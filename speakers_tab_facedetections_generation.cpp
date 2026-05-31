#include "speakers_tab.h"
#include "speaker_flow_debug.h"

#include "clip_serialization.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "json_io_utils.h"
#include "render_internal.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>

using namespace jcut::facedetections;

namespace {
bool uiAutomationEnabled()
{
    return qEnvironmentVariableIntValue("JCUT_UI_AUTOMATION") > 0;
}

void showAutomationAwareWarning(const QString& title, const QString& message)
{
    if (uiAutomationEnabled()) {
        qWarning().noquote() << title << ":" << message;
        return;
    }
    QMessageBox::warning(nullptr, title, message);
}

void showAutomationAwareInfo(const QString& title, const QString& message)
{
    if (uiAutomationEnabled()) {
        qInfo().noquote() << title << ":" << message;
        return;
    }
    QMessageBox::information(nullptr, title, message);
}

QString faceStreamSourceMediaPath(const TimelineClip& clip)
{
    const QString sourcePath = clip.filePath.trimmed();
    const QFileInfo sourceInfo(sourcePath);
    if (!sourcePath.isEmpty() &&
        sourceInfo.exists() &&
        (sourceInfo.isFile() || isImageSequencePath(sourcePath))) {
        return sourcePath;
    }
    return {};
}

QString faceStreamProxyMediaPath(const TimelineClip& clip)
{
    const QString proxyPath = playbackProxyPathForClip(clip);
    const QFileInfo proxyInfo(proxyPath);
    if (!proxyPath.trimmed().isEmpty() &&
        proxyInfo.exists() &&
        (proxyInfo.isFile() || isImageSequencePath(proxyPath))) {
        return proxyPath;
    }
    return {};
}

QString resolveMediaPathForFaceDetections(const TimelineClip& clip, bool useProxySource)
{
    return useProxySource ? faceStreamProxyMediaPath(clip) : faceStreamSourceMediaPath(clip);
}

bool readJsonObject(const QString& path, QJsonObject* objectOut, QString* errorOut = nullptr)
{
    return jcut::jsonio::readJsonFile(path, objectOut, errorOut);
}

QJsonObject existingFaceDetectionsRequest(const QString& requestPath)
{
    QJsonObject request;
    readJsonObject(requestPath, &request, nullptr);
    return request;
}

QString facedetectionsOffscreenExecutablePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeName = QStringLiteral("jcut_vulkan_facedetections_offscreen");
    const QString candidate = QDir(appDir).absoluteFilePath(exeName);
    if (QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QString buildCandidate = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("build/%1").arg(exeName));
    if (QFileInfo::exists(buildCandidate)) {
        return buildCandidate;
    }
    return candidate;
}

struct FaceDetectionsProcessResult {
    bool started = false;
    bool canceled = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QString standardOutput;
    QString standardError;
};

class ScopedAudioBackgroundDecodeSuppression {
public:
    explicit ScopedAudioBackgroundDecodeSuppression(const std::function<void(bool)>& setter)
        : m_setter(setter)
    {
        if (m_setter) {
            m_setter(true);
            m_active = true;
        }
    }

    ~ScopedAudioBackgroundDecodeSuppression()
    {
        if (m_active && m_setter) {
            m_setter(false);
        }
    }

    ScopedAudioBackgroundDecodeSuppression(const ScopedAudioBackgroundDecodeSuppression&) = delete;
    ScopedAudioBackgroundDecodeSuppression& operator=(const ScopedAudioBackgroundDecodeSuppression&) = delete;

private:
    std::function<void(bool)> m_setter;
    bool m_active = false;
};

FaceDetectionsProcessResult runFaceDetectionsGeneratorProcess(QWidget* parent,
                                                     const QString& program,
                                                     const QStringList& args,
                                                     bool livePreview)
{
    FaceDetectionsProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setWorkingDirectory(QFileInfo(program).absolutePath());
    process.setProcessChannelMode(QProcess::SeparateChannels);

    if (uiAutomationEnabled()) {
        process.start();
        result.started = process.waitForStarted(5000);
        if (!result.started) {
            result.standardError += process.errorString();
            return result;
        }
        process.waitForFinished(-1);
        result.exitCode = process.exitCode();
        result.exitStatus = process.exitStatus();
        result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
        result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
        return result;
    }

    if (livePreview) {
        QEventLoop loop;
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &loop, [&]() {
            result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
        });
        QObject::connect(&process, &QProcess::readyReadStandardError, &loop, [&]() {
            result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
        });
        QObject::connect(&process,
                         qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         &loop,
                         [&](int exitCode, QProcess::ExitStatus exitStatus) {
            result.exitCode = exitCode;
            result.exitStatus = exitStatus;
            loop.quit();
        });
        QObject::connect(&process, &QProcess::errorOccurred, &loop, [&]() {
            if (!result.standardError.endsWith(QLatin1Char('\n'))) {
                result.standardError += QLatin1Char('\n');
            }
            result.standardError += QStringLiteral("[process-error] %1\n").arg(process.errorString());
            if (process.state() == QProcess::NotRunning) {
                loop.quit();
            }
        });

        process.start();
        result.started = process.waitForStarted(5000);
        if (!result.started) {
            result.standardError += process.errorString();
            return result;
        }

        if (process.state() != QProcess::NotRunning) {
            loop.exec();
        } else {
            result.exitCode = process.exitCode();
            result.exitStatus = process.exitStatus();
        }
        result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
        result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
        return result;
    }

    QDialog progressDialog(parent);
    progressDialog.setWindowTitle(QStringLiteral("JCut DNN Detection + Continuity Generator"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.resize(760, 320);

    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);

    auto* statusLabel = new QLabel(
        livePreview
            ? QStringLiteral("Running detection and continuity generation with live preview in a background process...")
            : QStringLiteral("Running detection and continuity generation headless in a background process..."),
        &progressDialog);
    statusLabel->setWordWrap(true);
    progressLayout->addWidget(statusLabel);

    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, 0);
    progressLayout->addWidget(progressBar);

    auto* logView = new QPlainTextEdit(&progressDialog);
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(400);
    progressLayout->addWidget(logView, 1);

    auto appendLog = [logView](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        logView->moveCursor(QTextCursor::End);
        logView->insertPlainText(text);
        auto* bar = logView->verticalScrollBar();
        if (bar) {
            bar->setValue(bar->maximum());
        }
    };

    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch(1);
    auto* cancelRunButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    progressButtons->addWidget(cancelRunButton);
    progressLayout->addLayout(progressButtons);

    QObject::connect(cancelRunButton, &QPushButton::clicked, &progressDialog, [&]() {
        result.canceled = true;
        statusLabel->setText(QStringLiteral("Canceling detection and continuity generation..."));
        cancelRunButton->setEnabled(false);
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(1500)) {
                process.kill();
            }
        }
    });
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &progressDialog, [&]() {
        const QString chunk = QString::fromLocal8Bit(process.readAllStandardOutput());
        result.standardOutput += chunk;
        appendLog(chunk);
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &progressDialog, [&]() {
        const QString chunk = QString::fromLocal8Bit(process.readAllStandardError());
        result.standardError += chunk;
        appendLog(chunk);
    });
    QObject::connect(&process,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &progressDialog,
                     [&](int exitCode, QProcess::ExitStatus exitStatus) {
        result.exitCode = exitCode;
        result.exitStatus = exitStatus;
        progressDialog.accept();
    });
    QObject::connect(&process, &QProcess::errorOccurred, &progressDialog, [&](QProcess::ProcessError error) {
        appendLog(QStringLiteral("\n[process-error] %1\n").arg(static_cast<int>(error)));
    });

    process.start();
    result.started = process.waitForStarted(5000);
    if (!result.started) {
        result.standardError += process.errorString();
        return result;
    }

    progressDialog.exec();
    if (process.state() != QProcess::NotRunning) {
        process.waitForFinished(-1);
    }
    result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
    result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
    return result;
}

} // namespace

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    if (!activeCutMutable() || !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"),
                                   QStringLiteral("Select a clip first."));
        return;
    }

    const QString sourceMediaPath = faceStreamSourceMediaPath(*selectedClip);
    const QString proxyMediaPath = faceStreamProxyMediaPath(*selectedClip);
    if (sourceMediaPath.isEmpty() && proxyMediaPath.isEmpty()) {
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"),
                                   QStringLiteral("No playable media was found for this clip."));
        return;
    }

    DetectorRuntimeSettings detectorSettings;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(
        !sourceMediaPath.isEmpty() ? sourceMediaPath : proxyMediaPath);
    loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, nullptr);
    const FacestreamSourceScanRange scanRange = facedetectionsSourceAbsoluteScanRangeForClip(*selectedClip);
    if (!scanRange.valid) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Cannot run FaceDetections: %1").arg(scanRange.error));
        return;
    }
    const int64_t startFrame = scanRange.startFrame;
    const int64_t sourceEndFrameExclusive = scanRange.endFrameExclusive;
    const int64_t maxFrames = scanRange.frameCount;

    const FaceDetectionsPreflightDialogResult preflight =
        runFaceDetectionsPreflightDialog(
            &detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            detectorSettings.scrfdTargetSize,
            detectorSettingsPath,
            FaceDetectionsPreflightDialogOptions{
                QStringLiteral("JCut DNN Detection + Continuity Generator"),
                QStringLiteral("This flow runs raw face detection, then forms identity-agnostic continuity tracks, then imports those artefacts for the selected clip.\n\n"
                               "Detector: SCRFD ncnn Vulkan only. CPU detector fallback is not used."),
                QStringLiteral("Input defaults to source media. Enable proxy input explicitly if you want detection and continuity generation to scan the proxy instead. "
                               "Artifact: facedetections.part + tracks.idx/tracks.dat + detections.idx/detections.dat + continuity_facedetections.bin. Interrupted runs resume only when the input path still matches the checkpointed run."),
                QStringLiteral("Proceed"),
                QStringLiteral("Cancel"),
                QSize(760, 420),
                true,
                true,
                false,
                true,
                false,
                QStringLiteral("Apply selected clip grading during detection"),
                true,
                false,
                QStringLiteral("Restart from scratch (delete facedetections.part before launch)"),
                true,
                detectorSettings.useProxySource,
                QStringLiteral("Use proxy media as FaceDetections input"),
                true,
                2,
                1,
                10,
                QStringLiteral("Detector workers")
            });
    if (!preflight.accepted) {
        return;
    }
    if (!preflight.saveError.trimmed().isEmpty()) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            preflight.saveError.isEmpty()
                ? QStringLiteral("Failed to save detector settings before launch.")
                : preflight.saveError);
        return;
    }

    const QString mediaPath = resolveMediaPathForFaceDetections(*selectedClip, preflight.useProxySource);
    if (mediaPath.isEmpty()) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            preflight.useProxySource
                ? QStringLiteral("Proxy media was requested for detection/continuity input, but no playable proxy media was found for this clip.")
                : QStringLiteral("No playable source media was found for this clip."));
        return;
    }

    auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
        m_transcriptSession.transcriptPath(),
        selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id,
        QFileInfo(mediaPath).completeBaseName());
    if (debugRun.projectRoot.trimmed().isEmpty() || debugRun.runDir.trimmed().isEmpty()) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Cannot create a detection/continuity debug run because the active transcript path is unavailable."));
        return;
    }

    const QDir initialRunDir(debugRun.runDir);
    const QString initialRequestPath = initialRunDir.absoluteFilePath(
        QStringLiteral("%1_facedetections_request.json").arg(debugRun.videoStem));
    const QJsonObject initialRequest = existingFaceDetectionsRequest(initialRequestPath);
    const QString previousMediaPath = initialRequest.value(QStringLiteral("media_path")).toString().trimmed();
    const QString currentMediaPath = QFileInfo(mediaPath).absoluteFilePath();
    const bool initialRunMediaMismatch =
        !previousMediaPath.isEmpty() &&
        QFileInfo(previousMediaPath).absoluteFilePath() != currentMediaPath;
    const QDir initialArtifactDir(initialRunDir.absoluteFilePath(QStringLiteral("facedetections_artifact")));
    const bool initialRunHasCheckpoint =
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("facedetections.part")));
    const bool initialRunHasCompletedOutputs =
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("detections.idx"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("tracks.idx"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("continuity_facedetections.bin"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("summary.json")));
    const bool shouldForceFreshRun =
        debugRun.reusedExistingRun &&
        (initialRunMediaMismatch ||
         (initialRunHasCompletedOutputs && !initialRunHasCheckpoint));
    if (shouldForceFreshRun) {
        debugRun = speaker_flow_debug::createNewRunFrom(debugRun);
    }
    const QString artifactDir = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("facedetections_artifact"));
    QDir().mkpath(artifactDir);

    const QString requestPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facedetections_request.json").arg(debugRun.videoStem));
    const QString outputPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("continuity_facedetections.bin"));
    const QString detectionsPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("detections.idx"));
    const QString tracksPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("tracks.idx"));
    const QString summaryPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("summary.json"));
    const QString facedetectionsPartPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("facedetections.part"));
    const QString clipJsonPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("clip_input.json"));
    const QString indexPath = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("index.json"));

    if (preflight.restartFromScratch && QFileInfo::exists(facedetectionsPartPath) && !QFile::remove(facedetectionsPartPath)) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Failed to delete restart checkpoint before launch.\n\n%1")
                .arg(facedetectionsPartPath));
        return;
    }

    const int detectorWorkers = std::clamp(preflight.detectorWorkers, 1, 10);
    const int detectorPipelineSlots = detectorWorkers;

    QStringList args{
        mediaPath,
        QStringLiteral("--detector"), QStringLiteral("scrfd-ncnn-vulkan"),
        QStringLiteral("--params-file"), detectorSettingsPath,
        QStringLiteral("--stride"), QString::number(qMax(1, detectorSettings.stride)),
        QStringLiteral("--threshold"), QString::number(detectorSettings.threshold, 'f', 4),
        QStringLiteral("--nms-iou"), QString::number(detectorSettings.nmsIouThreshold, 'f', 4),
        QStringLiteral("--track-match-iou"), QString::number(detectorSettings.trackMatchIouThreshold, 'f', 4),
        QStringLiteral("--new-track-min-confidence"), QString::number(detectorSettings.newTrackMinConfidence, 'f', 4),
        QStringLiteral("--max-faces-per-frame"), QString::number(qMax(0, detectorSettings.maxFacesPerFrame)),
        QStringLiteral("--max-detections"), QString::number(qMax(1, detectorSettings.maxDetections)),
        QStringLiteral("--scrfd-model"), normalizeScrfdModelVariantId(detectorSettings.scrfdModelVariant),
        QStringLiteral("--scrfd-target-size"), QString::number(qMax(320, detectorSettings.scrfdTargetSize)),
        QStringLiteral("--start-frame"), QString::number(startFrame),
        QStringLiteral("--quiet"),
        QStringLiteral("--progress"),
        QStringLiteral("--detector-workers"), QString::number(detectorWorkers),
        QStringLiteral("--detector-pipeline-slots"), QString::number(detectorPipelineSlots),
        QStringLiteral("--out-dir"), artifactDir
    };
    args << (detectorSettings.primaryFaceOnly
                ? QStringLiteral("--primary-face-only")
                : QStringLiteral("--multi-face"));
    args << (detectorSettings.smallFaceFallback
                ? QStringLiteral("--small-face-fallback")
                : QStringLiteral("--no-small-face-fallback"));
    args << (detectorSettings.scrfdTiled
                ? QStringLiteral("--scrfd-tiling")
                : QStringLiteral("--no-scrfd-tiling"));
    args << QStringLiteral("--require-zero-copy");
    args << QStringLiteral("--control-window");
    args << (preflight.livePreview
                ? QStringLiteral("--preview-window")
                : QStringLiteral("--no-preview-window"));
    args << QStringLiteral("--no-preview-files");
    if (maxFrames > 0) {
        args << QStringLiteral("--max-frames") << QString::number(maxFrames);
    }
    if (preflight.applyClipGrading) {
        QString writeError;
        if (!jcut::jsonio::writeJsonFile(clipJsonPath, editor::clipToJson(*selectedClip), true, &writeError)) {
            showAutomationAwareWarning(
                QStringLiteral("JCut DNN FaceDetections Generator"),
                writeError.trimmed().isEmpty()
                    ? QStringLiteral("Failed to write clip grading input: %1").arg(clipJsonPath)
                    : writeError);
            return;
        }
        args << QStringLiteral("--clip-json") << clipJsonPath;
        args << QStringLiteral("--apply-clip-grading");
    }

    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("engine")] =
        QStringLiteral("jcut_vulkan_facedetections_offscreen_inprocess_scrfd_zero_copy_v1");
    request[QStringLiteral("execution_mode")] =
        QStringLiteral("inprocess_function");
    request[QStringLiteral("media_path")] = mediaPath;
    request[QStringLiteral("media_source_mode")] =
        preflight.useProxySource ? QStringLiteral("proxy") : QStringLiteral("source");
    request[QStringLiteral("clip_id")] = selectedClip->id;
    request[QStringLiteral("source_start_frame")] = static_cast<qint64>(startFrame);
    request[QStringLiteral("source_end_frame_exclusive")] = static_cast<qint64>(sourceEndFrameExclusive);
    request[QStringLiteral("max_frames")] = static_cast<qint64>(maxFrames);
    request[QStringLiteral("frame_domain")] =
        facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    request[QStringLiteral("detector_settings_file")] = detectorSettingsPath;
    request[QStringLiteral("detector_settings")] =
        detectorRuntimeSettingsToJson(
            detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            detectorSettings.scrfdTargetSize);
    request[QStringLiteral("detector_workers")] = detectorWorkers;
    request[QStringLiteral("detector_pipeline_slots")] = detectorPipelineSlots;
    request[QStringLiteral("artifact_out_dir")] = artifactDir;
    request[QStringLiteral("facedetections_part")] = facedetectionsPartPath;
    request[QStringLiteral("detections_bin")] = detectionsPath;
    request[QStringLiteral("tracks_bin")] = tracksPath;
    request[QStringLiteral("continuity_facedetections_bin")] = outputPath;
    request[QStringLiteral("summary_json")] = summaryPath;
    request[QStringLiteral("clip_json")] = preflight.applyClipGrading ? clipJsonPath : QString();
    request[QStringLiteral("apply_clip_grading")] = preflight.applyClipGrading;
    request[QStringLiteral("arguments")] = args.join(QLatin1Char(' '));
    QString requestWriteError;
    if (!jcut::jsonio::writeJsonFile(requestPath, request, true, &requestWriteError)) {
        qWarning().noquote() << "Failed to write detection/continuity request file:" << requestWriteError;
    }

    const QString generatorProgram = facedetectionsOffscreenExecutablePath();
    if (!QFileInfo::exists(generatorProgram)) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Detection/continuity generator executable was not found.\n\nExpected: %1")
                .arg(generatorProgram));
        return;
    }
    const FaceDetectionsProcessResult processResult =
        [&]() {
            ScopedAudioBackgroundDecodeSuppression suppressAudioDecode(
                m_speakerDeps.setAudioBackgroundDecodeSuppressed);
            return runFaceDetectionsGeneratorProcess(nullptr, generatorProgram, args, true);
        }();
    if (!processResult.started) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Failed to start detection/continuity generator.\n\n%1")
                .arg(processResult.standardError.trimmed().isEmpty()
                         ? QStringLiteral("Unknown process start failure.")
                         : processResult.standardError.trimmed()));
        return;
    }
    if (processResult.canceled) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("canceled"),
            QStringLiteral("Detection and continuity generation canceled by user."),
            {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        return;
    }

    const bool processOk =
        processResult.exitStatus == QProcess::NormalExit &&
        processResult.exitCode == 0;
    if (!processOk) {
        const QString message =
            QStringLiteral("JCut DNN Detection + Continuity Generator failed (exit code %1).\n\n%2")
                .arg(processResult.exitCode)
                .arg(processResult.standardError.trimmed().isEmpty()
                         ? processResult.standardOutput.trimmed()
                         : processResult.standardError.trimmed());
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            message, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), message);
        return;
    }

    QString parseError;
    QJsonObject generatedArtifact;
    if (!jcut::jsonio::readBinaryJsonObject(outputPath, &generatedArtifact, 0x4A435554, 1, &parseError) &&
        !readJsonObject(outputPath, &generatedArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            parseError, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), parseError);
        return;
    }

    QJsonObject continuityRoot = continuityRootForClip(generatedArtifact, QStringLiteral("facedetections-offscreen-source"));
    QJsonObject rawTracksArtifact;
    if (!jcut::facedetections::readBinaryJsonObject(tracksPath, &rawTracksArtifact, &parseError) &&
        !readJsonObject(tracksPath, &rawTracksArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            parseError, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), parseError);
        return;
    }

    const QJsonArray rawTracks = rawTracksArtifact.value(QStringLiteral("tracks")).toArray();
    const QJsonArray rawFrames = rawTracksArtifact.value(QStringLiteral("frames")).toArray();
    const int rawFramesCount = rawFrames.isEmpty() && QFileInfo::exists(detectionsPath)
        ? -1
        : rawFrames.size();
    continuityRoot[QStringLiteral("raw_tracks_artifact_path")] = tracksPath;
    continuityRoot[QStringLiteral("raw_frames_artifact_path")] = detectionsPath;
    continuityRoot[QStringLiteral("continuity_artifact_path")] = outputPath;
    continuityRoot[QStringLiteral("raw_tracks_count")] = rawTracks.size();
    continuityRoot[QStringLiteral("raw_frames_count")] = rawFramesCount;
    continuityRoot[QStringLiteral("raw_tracks_schema")] = rawTracksArtifact.value(QStringLiteral("schema")).toString();
    continuityRoot[QStringLiteral("raw_frames_schema")] =
        QStringLiteral("jcut_facedetections_offscreen_detections_v1");
    const QString rawTracksFrameDomain =
        rawTracksArtifact.value(QStringLiteral("frame_domain")).toString(
            facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute));
    const QString rawFramesFrameDomain = rawTracksFrameDomain;
    continuityRoot[QStringLiteral("raw_tracks_frame_domain")] = rawTracksFrameDomain;
    continuityRoot[QStringLiteral("raw_frames_frame_domain")] = rawFramesFrameDomain;
    continuityRoot[QStringLiteral("streams_frame_domain")] =
        rawTracksFrameDomain;
    if (!continuityRoot.contains(QStringLiteral("detector_mode"))) {
        continuityRoot[QStringLiteral("detector_mode")] = rawTracksArtifact.value(QStringLiteral("backend")).toString();
    }

    if (rawTracks.isEmpty() && rawFrames.isEmpty()) {
        const QString noDetectionsMessage = QStringLiteral("Generated artifact contains no raw face detections for this clip.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            noDetectionsMessage, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), noDetectionsMessage);
        return;
    }
    continuityRoot[QStringLiteral("run_id")] = debugRun.runId;
    continuityRoot[QStringLiteral("imported_from_artifact_dir")] = artifactDir;
    continuityRoot[QStringLiteral("facedetections_part")] = facedetectionsPartPath;
    continuityRoot[QStringLiteral("summary_json")] = summaryPath;
    continuityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    const QString clipId = selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id.trimmed();
    continuityRoot[QStringLiteral("clip_id")] = clipId;
    continuityRoot[QStringLiteral("processed_artifact_path")] =
        editor::TranscriptEngine().facedetectionsProcessedArtifactPath(m_transcriptSession.transcriptPath());
    QJsonObject artifactRoot;
    const bool saved = jcut::facedetections::saveContinuityArtifact(
        m_transcriptSession.transcriptPath(),
        clipId,
        continuityRoot,
        &artifactRoot);
    bool savedProcessed = true;
    if (saved && !rawTracks.isEmpty()) {
        savedProcessed = jcut::facedetections::saveProcessedContinuityArtifact(
            m_transcriptSession.transcriptPath(),
            clipId,
            continuityRoot,
            m_transcriptSession.rootObject(),
            nullptr);
    }
    QString statusMessage = saved
        ? QStringLiteral("Imported generated detections and continuity tracks.")
        : QStringLiteral("Generated detections and continuity tracks, but failed to save the transcript artifact.");
    if (saved && rawTracks.isEmpty()) {
        statusMessage += QStringLiteral(" Track processing now runs in a later step.");
    }
    if (saved && !savedProcessed) {
        statusMessage += QStringLiteral(" Processed continuity sidecar rebuild failed.");
    }
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"),
        (saved && savedProcessed) ? QStringLiteral("ok") : QStringLiteral("error"),
        statusMessage, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});

    if (!saved) {
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), statusMessage);
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    clearFaceDetectionsDerivedCaches();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    showAutomationAwareInfo(
        QStringLiteral("JCut DNN Detection + Continuity Generator"),
        QStringLiteral("Imported raw detections and continuity tracks.\n\nFrames: %1\nTracks: %2\nArtifact: %3")
            .arg(rawFramesCount < 0 ? QStringLiteral("referenced") : QString::number(rawFramesCount))
            .arg(rawTracks.size())
            .arg(artifactDir));
    refresh();
}
