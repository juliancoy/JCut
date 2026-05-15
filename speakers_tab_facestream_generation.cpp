#include "speakers_tab.h"
#include "speaker_flow_debug.h"

#include "facestream_runtime.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "render_internal.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

using namespace jcut::facestream;

namespace {
constexpr int kFaceStreamScrfdTargetSize = 640;

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

QString facestreamOffscreenExecutablePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeName = QStringLiteral("jcut_vulkan_facestream_offscreen");
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

struct FaceStreamProcessResult {
    bool started = false;
    bool canceled = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QString standardOutput;
    QString standardError;
};

FaceStreamProcessResult runFaceStreamGeneratorProcess(QWidget* parent,
                                                     const QString& program,
                                                     const QStringList& args,
                                                     bool livePreview)
{
    FaceStreamProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setWorkingDirectory(QFileInfo(program).absolutePath());
    process.setProcessChannelMode(QProcess::SeparateChannels);

    QDialog progressDialog(parent);
    progressDialog.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.resize(760, 320);

    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);

    auto* statusLabel = new QLabel(
        livePreview
            ? QStringLiteral("Running FaceStream generator with live preview in a background process...")
            : QStringLiteral("Running FaceStream generator headless in a background process..."),
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
        statusLabel->setText(QStringLiteral("Canceling FaceStream generator..."));
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
    if (debugRun.projectRoot.trimmed().isEmpty() || debugRun.runDir.trimmed().isEmpty()) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("JCut DNN FaceStream Generator"),
            QStringLiteral("Cannot create a FaceStream debug run because the active transcript path is unavailable."));
        return;
    }
    const bool transcriptHasFacestreamSidecar =
        facestreamSidecarExistsForClipFile(selectedClip->filePath);
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
    const bool shouldForceFreshRun =
        debugRun.reusedExistingRun &&
        (!transcriptHasFacestreamSidecar || initialRunLooksLegacyOnly);
    if (shouldForceFreshRun) {
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
    auto* livePreviewCheckbox = new QCheckBox(QStringLiteral("Show live preview"), &preflightDialog);
    livePreviewCheckbox->setChecked(true);
    layout->addWidget(livePreviewCheckbox);
    DetectorSettingsPanel detectorPanel =
        createDetectorSettingsPanel(
            &detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            kFaceStreamScrfdTargetSize,
            detectorSettingsPath,
            &preflightDialog);
    layout->addWidget(detectorPanel.widget);
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
        args << QStringLiteral("--require-zero-copy");
        args << (livePreviewCheckbox->isChecked()
                    ? QStringLiteral("--preview-window")
                    : QStringLiteral("--no-preview-window"));
        args << QStringLiteral("--no-preview-files");
        if (maxFrames > 0) {
            args << QStringLiteral("--max-frames") << QString::number(maxFrames);
        }
        return args;
    };

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
    args << QStringLiteral("--require-zero-copy");
    args << (livePreviewCheckbox->isChecked()
                ? QStringLiteral("--preview-window")
                : QStringLiteral("--no-preview-window"));
    args << QStringLiteral("--no-preview-files");
    if (maxFrames > 0) {
        args << QStringLiteral("--max-frames") << QString::number(maxFrames);
    }

    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("engine")] =
        QStringLiteral("jcut_vulkan_facestream_offscreen_inprocess_scrfd_zero_copy_v1");
    request[QStringLiteral("execution_mode")] =
        QStringLiteral("inprocess_function");
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

    const QString generatorProgram = facestreamOffscreenExecutablePath();
    if (!QFileInfo::exists(generatorProgram)) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("JCut DNN FaceStream Generator"),
            QStringLiteral("FaceStream generator executable was not found.\n\nExpected: %1")
                .arg(generatorProgram));
        return;
    }
    const FaceStreamProcessResult processResult =
        runFaceStreamGeneratorProcess(nullptr, generatorProgram, args, livePreviewCheckbox->isChecked());
    if (!processResult.started) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("JCut DNN FaceStream Generator"),
            QStringLiteral("Failed to start FaceStream generator.\n\n%1")
                .arg(processResult.standardError.trimmed().isEmpty()
                         ? QStringLiteral("Unknown process start failure.")
                         : processResult.standardError.trimmed()));
        return;
    }
    if (processResult.canceled) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_facestream"), QStringLiteral("canceled"),
            QStringLiteral("FaceStream generation canceled by user."),
            {requestPath, facestreamPartPath, tracksPath, outputPath, summaryPath});
        return;
    }

    const bool processOk =
        processResult.exitStatus == QProcess::NormalExit &&
        processResult.exitCode == 0;
    if (!processOk) {
        const QString message =
            QStringLiteral("JCut DNN FaceStream Generator failed (exit code %1).\n\n%2")
                .arg(processResult.exitCode)
                .arg(processResult.standardError.trimmed().isEmpty()
                         ? processResult.standardOutput.trimmed()
                         : processResult.standardError.trimmed());
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
