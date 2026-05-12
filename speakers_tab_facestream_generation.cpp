#include "speakers_tab.h"
#include "speaker_flow_debug.h"

#include "facestream_runtime.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "render_internal.h"
#include "transcript_engine.h"
#include "vulkan_facestream_offscreen_runner.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

using namespace jcut::facestream;

namespace {
constexpr int kFaceStreamScrfdTargetSize = 640;

QString resolveFaceStreamGeneratorBinary()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(appDir).absoluteFilePath(QStringLiteral("jcut_vulkan_facestream_offscreen")),
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("build/jcut_vulkan_facestream_offscreen")),
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("jcut_vulkan_facestream_offscreen")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../jcut_vulkan_facestream_offscreen"))
    };
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

QString resolveMediaPathForFaceStream(const TimelineClip& clip)
{
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
    return {};
}

bool readJsonObject(const QString& path, QJsonObject* objectOut, QString* errorOut = nullptr)
{
    if (objectOut) {
        *objectOut = QJsonObject{};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1").arg(path);
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to parse %1: %2").arg(path, parseError.errorString());
        }
        return false;
    }
    if (objectOut) {
        *objectOut = doc.object();
    }
    return true;
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

    const QString mediaPath = resolveMediaPathForFaceStream(*selectedClip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                             QStringLiteral("No playable media was found for this clip."));
        return;
    }

    auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
        m_loadedTranscriptPath,
        selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id,
        QFileInfo(mediaPath).completeBaseName());
    const QDir initialRunDir(debugRun.runDir);
    const QDir initialArtifactDir(initialRunDir.absoluteFilePath(QStringLiteral("facestream_artifact")));
    const bool initialRunHasArtifactState =
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("facestream.part"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("tracks.bin"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("continuity_facestream.bin"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("summary.json")));
    const bool initialRunLooksLegacyOnly =
        !initialRunHasArtifactState &&
        !initialRunDir.entryList(QStringList{QStringLiteral("*_continuity_facestream_request.json")},
                                 QDir::Files).isEmpty();
    if (initialRunLooksLegacyOnly) {
        debugRun = speaker_flow_debug::createNewRunFrom(debugRun);
    }
    const QString artifactDir = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("facestream_artifact"));
    QDir().mkpath(artifactDir);

    DetectorRuntimeSettings detectorSettings;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(mediaPath);
    loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, nullptr);
    const int64_t startFrame = qMax<int64_t>(0, selectedClip->sourceInFrame);
    const int64_t maxFrames = qMax<int64_t>(0, selectedClip->durationFrames);

    QDialog preflightDialog;
    preflightDialog.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
    preflightDialog.setWindowFlag(Qt::Window, true);
    preflightDialog.resize(760, 420);
    auto* layout = new QVBoxLayout(&preflightDialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* infoLabel = new QLabel(
        QStringLiteral("This flow launches the generated FaceStream artifact pipeline, then imports its continuity tracks for the selected clip.\n\n"
                       "Detector: SCRFD ncnn Vulkan only. CPU detector fallback is not used."),
        &preflightDialog);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);
    auto* artifactLabel = new QLabel(
        QStringLiteral("Artifact: facestream.part + tracks.bin + continuity_facestream.bin. Interrupted runs resume from facestream.part."),
        &preflightDialog);
    artifactLabel->setWordWrap(true);
    artifactLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    layout->addWidget(artifactLabel);
    DetectorSettingsPanel detectorPanel =
        createDetectorSettingsPanel(
            &detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            kFaceStreamScrfdTargetSize,
            detectorSettingsPath,
            &preflightDialog);
    layout->addWidget(detectorPanel.widget);
    auto* previewCheckBox = new QCheckBox(QStringLiteral("Enable Preview (window + preview files)"), &preflightDialog);
    previewCheckBox->setChecked(false);
    previewCheckBox->setToolTip(
        QStringLiteral("When enabled, runs the generator with interactive preview output instead of offscreen-only mode."));
    layout->addWidget(previewCheckBox);
    auto* allowCpuFallbackCheckBox = new QCheckBox(QStringLiteral("Allow CPU Upload Fallback (explicit)"), &preflightDialog);
    allowCpuFallbackCheckBox->setChecked(false);
    allowCpuFallbackCheckBox->setToolTip(
        QStringLiteral("Unchecked = strict zero-copy (hard fail). Checked = permit decoder CPU-image upload fallback."));
    layout->addWidget(allowCpuFallbackCheckBox);
    auto* isolatedProcessCheckBox = new QCheckBox(QStringLiteral("Run In Isolated Worker Process"), &preflightDialog);
    isolatedProcessCheckBox->setChecked(false);
    isolatedProcessCheckBox->setToolTip(
        QStringLiteral("Uses external jcut_vulkan_facestream_offscreen binary. Slower startup, but isolates crashes from the editor."));
    layout->addWidget(isolatedProcessCheckBox);
    auto* copyWorkerCommandCheckBox = new QCheckBox(QStringLiteral("Copy Worker Command On Proceed"), &preflightDialog);
    copyWorkerCommandCheckBox->setChecked(false);
    copyWorkerCommandCheckBox->setEnabled(false);
    copyWorkerCommandCheckBox->setToolTip(
        QStringLiteral("Copies the exact external command line to clipboard when isolated worker mode is selected."));
    layout->addWidget(copyWorkerCommandCheckBox);
    QObject::connect(isolatedProcessCheckBox, &QCheckBox::toggled, &preflightDialog, [copyWorkerCommandCheckBox](bool checked) {
        copyWorkerCommandCheckBox->setEnabled(checked);
        if (!checked) {
            copyWorkerCommandCheckBox->setChecked(false);
        }
    });
    auto* commandLabel = new QLabel(QStringLiteral("Command Preview"), &preflightDialog);
    commandLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    layout->addWidget(commandLabel);
    auto* commandEdit = new QPlainTextEdit(&preflightDialog);
    commandEdit->setReadOnly(true);
    commandEdit->setMaximumBlockCount(10);
    commandEdit->setMinimumHeight(90);
    commandEdit->setPlaceholderText(QStringLiteral("Command will appear here."));
    layout->addWidget(commandEdit);
    auto* commandButtons = new QHBoxLayout;
    commandButtons->addStretch(1);
    auto* copyCommandButton = new QPushButton(QStringLiteral("Copy Command"), &preflightDialog);
    commandButtons->addWidget(copyCommandButton);
    layout->addLayout(commandButtons);
    auto buildArgsForPreflight = [&]() {
        QStringList args{
            mediaPath,
            QStringLiteral("--detector"), QStringLiteral("scrfd-ncnn-vulkan"),
            QStringLiteral("--stride"), QString::number(qMax(1, detectorSettings.stride)),
            QStringLiteral("--threshold"), QString::number(detectorSettings.threshold, 'f', 4),
            QStringLiteral("--nms-iou"), QString::number(detectorSettings.nmsIouThreshold, 'f', 4),
            QStringLiteral("--track-match-iou"), QString::number(detectorSettings.trackMatchIouThreshold, 'f', 4),
            QStringLiteral("--new-track-min-confidence"), QString::number(detectorSettings.newTrackMinConfidence, 'f', 4),
            QStringLiteral("--max-faces-per-frame"), QString::number(qMax(0, detectorSettings.maxFacesPerFrame)),
            QStringLiteral("--scrfd-target-size"), QString::number(kFaceStreamScrfdTargetSize),
            QStringLiteral("--start-frame"), QString::number(startFrame),
            QStringLiteral("--quiet"),
            QStringLiteral("--progress"),
            QStringLiteral("--out-dir"), artifactDir
        };
        args << (detectorSettings.primaryFaceOnly
                    ? QStringLiteral("--primary-face-only")
                    : QStringLiteral("--multi-face"));
        args << (detectorSettings.smallFaceFallback
                    ? QStringLiteral("--small-face-fallback")
                    : QStringLiteral("--no-small-face-fallback"));
        if (allowCpuFallbackCheckBox->isChecked()) {
            args << QStringLiteral("--allow-cpu-upload-fallback");
        } else {
            args << QStringLiteral("--require-zero-copy");
        }
        if (previewCheckBox->isChecked()) {
            args << QStringLiteral("--preview-window")
                 << QStringLiteral("--preview-files");
        } else {
            args << QStringLiteral("--no-preview-window")
                 << QStringLiteral("--no-preview-files");
        }
        if (maxFrames > 0) {
            args << QStringLiteral("--max-frames") << QString::number(maxFrames);
        }
        return args;
    };
    auto shellQuote = [](const QString& token) {
        QString out = token;
        out.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
        return QStringLiteral("'%1'").arg(out);
    };
    auto refreshCommandPreview = [&]() {
        QString executable = QStringLiteral("jcut_vulkan_facestream_offscreen");
        if (isolatedProcessCheckBox->isChecked()) {
            const QString resolved = resolveFaceStreamGeneratorBinary();
            if (!resolved.isEmpty()) {
                executable = resolved;
            }
        }
        const QStringList args = buildArgsForPreflight();
        QStringList tokens;
        tokens << executable;
        tokens << args;
        QStringList quoted;
        quoted.reserve(tokens.size());
        for (const QString& token : tokens) {
            quoted << shellQuote(token);
        }
        commandEdit->setPlainText(quoted.join(QLatin1Char(' ')));
    };
    QObject::connect(previewCheckBox, &QCheckBox::toggled, &preflightDialog, [refreshCommandPreview](bool) {
        refreshCommandPreview();
    });
    QObject::connect(allowCpuFallbackCheckBox, &QCheckBox::toggled, &preflightDialog, [refreshCommandPreview](bool) {
        refreshCommandPreview();
    });
    QObject::connect(isolatedProcessCheckBox, &QCheckBox::toggled, &preflightDialog, [refreshCommandPreview](bool) {
        refreshCommandPreview();
    });
    QObject::connect(copyCommandButton, &QPushButton::clicked, &preflightDialog, [commandEdit]() {
        if (QClipboard* clipboard = QApplication::clipboard()) {
            clipboard->setText(commandEdit->toPlainText());
        }
    });
    refreshCommandPreview();

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
    const bool previewEnabled = previewCheckBox->isChecked();
    const bool allowCpuUploadFallback = allowCpuFallbackCheckBox->isChecked();
    const bool runIsolatedWorker = isolatedProcessCheckBox->isChecked();
    const bool copyWorkerCommand = copyWorkerCommandCheckBox->isChecked();
    QString saveDetectorSettingsError;
    if (!saveDetectorRuntimeSettingsFile(
            detectorSettingsPath,
            detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            kFaceStreamScrfdTargetSize,
            &saveDetectorSettingsError)) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("JCut DNN FaceStream Generator"),
            saveDetectorSettingsError.isEmpty()
                ? QStringLiteral("Failed to save detector settings before launch.")
                : saveDetectorSettingsError);
        return;
    }

    QString generatorPath;
    if (runIsolatedWorker) {
        generatorPath = resolveFaceStreamGeneratorBinary();
        if (generatorPath.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                                 QStringLiteral("Could not find jcut_vulkan_facestream_offscreen for isolated worker mode."));
            return;
        }
    }

    const QString requestPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facestream_request.json").arg(debugRun.videoStem));
    const QString outputPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("continuity_facestream.bin"));
    const QString tracksPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("tracks.bin"));
    const QString summaryPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("summary.json"));
    const QString facestreamPartPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("facestream.part"));
    const QString indexPath = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("index.json"));

    QStringList args{
        mediaPath,
        QStringLiteral("--detector"), QStringLiteral("scrfd-ncnn-vulkan"),
        QStringLiteral("--stride"), QString::number(qMax(1, detectorSettings.stride)),
        QStringLiteral("--threshold"), QString::number(detectorSettings.threshold, 'f', 4),
        QStringLiteral("--nms-iou"), QString::number(detectorSettings.nmsIouThreshold, 'f', 4),
        QStringLiteral("--track-match-iou"), QString::number(detectorSettings.trackMatchIouThreshold, 'f', 4),
        QStringLiteral("--new-track-min-confidence"), QString::number(detectorSettings.newTrackMinConfidence, 'f', 4),
        QStringLiteral("--max-faces-per-frame"), QString::number(qMax(0, detectorSettings.maxFacesPerFrame)),
        QStringLiteral("--scrfd-target-size"), QString::number(kFaceStreamScrfdTargetSize),
        QStringLiteral("--start-frame"), QString::number(startFrame),
        QStringLiteral("--quiet"),
        QStringLiteral("--progress"),
        QStringLiteral("--out-dir"), artifactDir
    };
    args << (detectorSettings.primaryFaceOnly
                ? QStringLiteral("--primary-face-only")
                : QStringLiteral("--multi-face"));
    args << (detectorSettings.smallFaceFallback
                ? QStringLiteral("--small-face-fallback")
                : QStringLiteral("--no-small-face-fallback"));
    if (allowCpuUploadFallback) {
        args << QStringLiteral("--allow-cpu-upload-fallback");
    } else {
        args << QStringLiteral("--require-zero-copy");
    }
    if (previewEnabled) {
        args << QStringLiteral("--preview-window")
             << QStringLiteral("--preview-files");
    } else {
        args << QStringLiteral("--no-preview-window")
             << QStringLiteral("--no-preview-files");
    }
    if (maxFrames > 0) {
        args << QStringLiteral("--max-frames") << QString::number(maxFrames);
    }
    if (runIsolatedWorker && copyWorkerCommand) {
        if (QClipboard* clipboard = QApplication::clipboard()) {
            clipboard->setText(commandEdit->toPlainText());
        }
    }

    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("engine")] = runIsolatedWorker
        ? QStringLiteral("jcut_vulkan_facestream_offscreen_worker_scrfd_zero_copy_v1")
        : QStringLiteral("jcut_vulkan_facestream_offscreen_inprocess_scrfd_zero_copy_v1");
    request[QStringLiteral("execution_mode")] = runIsolatedWorker
        ? QStringLiteral("isolated_worker_binary")
        : QStringLiteral("inprocess_function");
    if (runIsolatedWorker) {
        request[QStringLiteral("generator_binary")] = generatorPath;
    }
    request[QStringLiteral("media_path")] = mediaPath;
    request[QStringLiteral("clip_id")] = selectedClip->id;
    request[QStringLiteral("source_start_frame")] = static_cast<qint64>(startFrame);
    request[QStringLiteral("max_frames")] = static_cast<qint64>(maxFrames);
    request[QStringLiteral("detector_settings_file")] = detectorSettingsPath;
    request[QStringLiteral("detector_settings")] =
        detectorRuntimeSettingsToJson(
            detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            kFaceStreamScrfdTargetSize);
    request[QStringLiteral("artifact_out_dir")] = artifactDir;
    request[QStringLiteral("facestream_part")] = facestreamPartPath;
    request[QStringLiteral("tracks_bin")] = tracksPath;
    request[QStringLiteral("continuity_facestream_bin")] = outputPath;
    request[QStringLiteral("summary_json")] = summaryPath;
    request[QStringLiteral("arguments")] = args.join(QLatin1Char(' '));
    QFile reqFile(requestPath);
    if (reqFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reqFile.write(QJsonDocument(request).toJson(QJsonDocument::Indented));
        reqFile.close();
    }

    QDialog progressDialog;
    progressDialog.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.resize(520, 160);
    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);
    auto* statusLabel = new QLabel(
        runIsolatedWorker
            ? QStringLiteral("Generating resumable SCRFD zero-copy FaceStream artifact in isolated worker process...")
            : QStringLiteral("Generating resumable SCRFD zero-copy FaceStream artifact in-process..."),
        &progressDialog);
    statusLabel->setWordWrap(true);
    progressLayout->addWidget(statusLabel);
    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, 0);
    progressLayout->addWidget(progressBar);
    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch(1);
    auto* cancelRunButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    cancelRunButton->setEnabled(runIsolatedWorker);
    progressButtons->addWidget(cancelRunButton);
    progressLayout->addLayout(progressButtons);
    progressDialog.show();
    QApplication::processEvents();
    int runExitCode = -1;
    if (runIsolatedWorker) {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        if (!previewEnabled) {
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        }
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(QFileInfo(generatorPath).absolutePath());
        bool canceled = false;
        QObject::connect(cancelRunButton, &QPushButton::clicked, [&]() {
            canceled = true;
            cancelRunButton->setEnabled(false);
            statusLabel->setText(QStringLiteral("Canceling worker process..."));
            process.terminate();
        });
        process.start(generatorPath, args);
        if (!process.waitForStarted(3000)) {
            progressDialog.close();
            const QString message = QStringLiteral("Failed to start isolated worker: %1").arg(process.errorString());
            QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), message);
            return;
        }
        while (!process.waitForFinished(100)) {
            QApplication::processEvents();
            if (canceled && process.state() != QProcess::NotRunning && !process.waitForFinished(1000)) {
                process.kill();
            }
        }
        runExitCode = (process.exitStatus() == QProcess::NormalExit) ? process.exitCode() : -1;
    } else {
        runExitCode = runVulkanFacestreamOffscreen(args);
    }
    progressDialog.close();

    const bool processOk = runExitCode == 0;
    if (!processOk) {
        const QString message =
            QStringLiteral("JCut DNN FaceStream Generator failed (exit code %1).")
                .arg(runExitCode);
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_facestream"), QStringLiteral("error"),
            message, {requestPath, facestreamPartPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), message);
        return;
    }

    QString parseError;
    QJsonObject generatedArtifact;
    if (!jcut::facestream::readBinaryJsonObject(outputPath, &generatedArtifact, &parseError) &&
        !readJsonObject(outputPath, &generatedArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_facestream"), QStringLiteral("error"),
            parseError, {requestPath, facestreamPartPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), parseError);
        return;
    }

    const QJsonObject generatedByClip = generatedArtifact.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
    QJsonObject continuityRoot = generatedByClip.value(QStringLiteral("facestream-offscreen-source")).toObject();
    QJsonObject rawTracksArtifact;
    if (!jcut::facestream::readBinaryJsonObject(tracksPath, &rawTracksArtifact, &parseError) &&
        !readJsonObject(tracksPath, &rawTracksArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_facestream"), QStringLiteral("error"),
            parseError, {requestPath, facestreamPartPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), parseError);
        return;
    }

    continuityRoot[QStringLiteral("raw_tracks")] = rawTracksArtifact.value(QStringLiteral("tracks")).toArray();
    continuityRoot[QStringLiteral("raw_frames")] = rawTracksArtifact.value(QStringLiteral("frames")).toArray();
    continuityRoot[QStringLiteral("raw_schema")] = rawTracksArtifact.value(QStringLiteral("schema")).toString();
    if (!continuityRoot.contains(QStringLiteral("detector_mode"))) {
        continuityRoot[QStringLiteral("detector_mode")] = rawTracksArtifact.value(QStringLiteral("backend")).toString();
    }

    const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(continuityRoot);
    if (streams.isEmpty()) {
        const QString noTracksMessage = QStringLiteral("Generated FaceStream artifact contains no usable raw FaceStream tracks for this clip.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_facestream"), QStringLiteral("error"),
            noTracksMessage, {requestPath, facestreamPartPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), noTracksMessage);
        return;
    }
    continuityRoot[QStringLiteral("run_id")] = debugRun.runId;
    continuityRoot[QStringLiteral("imported_from_artifact_dir")] = artifactDir;
    continuityRoot[QStringLiteral("facestream_part")] = facestreamPartPath;
    continuityRoot[QStringLiteral("summary_json")] = summaryPath;
    continuityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    const QString clipId = selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id.trimmed();
    continuityRoot[QStringLiteral("clip_id")] = clipId;
    continuityRoot[QStringLiteral("processed_artifact_path")] =
        editor::TranscriptEngine().facestreamProcessedArtifactPath(m_loadedTranscriptPath);
    QJsonObject artifactRoot;
    const bool saved = jcut::facestream::saveContinuityArtifact(
        m_loadedTranscriptPath,
        clipId,
        continuityRoot,
        &artifactRoot);
    bool savedProcessed = false;
    if (saved) {
        savedProcessed = jcut::facestream::saveProcessedContinuityArtifact(
            m_loadedTranscriptPath,
            clipId,
            continuityRoot,
            m_loadedTranscriptDoc.object(),
            nullptr);
    }
    QString statusMessage = saved
        ? QStringLiteral("Imported generated FaceStream artifact.")
        : QStringLiteral("Generated FaceStream artifact, but failed to save transcript artifact.");
    if (saved && !savedProcessed) {
        statusMessage += QStringLiteral(" Processed FaceStream sidecar rebuild failed.");
    }
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_loadedTranscriptPath, QStringLiteral("stage_6_facestream"),
        (saved && savedProcessed) ? QStringLiteral("ok") : QStringLiteral("error"),
        statusMessage, {requestPath, facestreamPartPath, tracksPath, outputPath, summaryPath});

    if (!saved) {
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), statusMessage);
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
        QStringLiteral("Imported %1 FaceStream path(s).\n\nArtifact: %2")
            .arg(streams.size())
            .arg(artifactDir));
    refresh();
}
