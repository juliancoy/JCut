#include "speakers_tab.h"
#include "speaker_flow_debug.h"

#include "boxstream_runtime.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "render_internal.h"

#include <QApplication>
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
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

using namespace jcut::boxstream;

namespace {

QString resolveFaceStreamGeneratorBinary()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(appDir).absoluteFilePath(QStringLiteral("jcut_vulkan_boxstream_offscreen")),
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("build/jcut_vulkan_boxstream_offscreen")),
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("jcut_vulkan_boxstream_offscreen")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../jcut_vulkan_boxstream_offscreen"))
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

QString appendProcessText(QPlainTextEdit* edit, const QString& buffered, const QByteArray& bytes)
{
    QString combined = buffered + QString::fromUtf8(bytes);
    combined.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = combined.split(QLatin1Char('\n'));
    QString remainder;
    const int completeCount = combined.endsWith(QLatin1Char('\n')) ? lines.size() : lines.size() - 1;
    for (int i = 0; i < completeCount; ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        edit->appendPlainText(line);
    }
    if (!combined.endsWith(QLatin1Char('\n')) && !lines.isEmpty()) {
        remainder = lines.constLast();
    }
    QTextCursor cursor = edit->textCursor();
    cursor.movePosition(QTextCursor::End);
    edit->setTextCursor(cursor);
    return remainder;
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
    const QString generatorPath = resolveFaceStreamGeneratorBinary();
    if (generatorPath.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                             QStringLiteral("Could not find jcut_vulkan_boxstream_offscreen. Build that target first."));
        return;
    }

    QDialog preflightDialog;
    preflightDialog.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
    preflightDialog.setWindowFlag(Qt::Window, true);
    preflightDialog.resize(620, 230);
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
        QStringLiteral("Artifact: facestream.ndjson + tracks.json + continuity_boxstream.json. Interrupted runs resume from facestream.ndjson."),
        &preflightDialog);
    artifactLabel->setWordWrap(true);
    artifactLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    layout->addWidget(artifactLabel);
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

    auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
        m_loadedTranscriptPath,
        selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id,
        QFileInfo(mediaPath).completeBaseName());
    const QDir initialRunDir(debugRun.runDir);
    const QDir initialArtifactDir(initialRunDir.absoluteFilePath(QStringLiteral("facestream_artifact")));
    const bool initialRunHasArtifactState =
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("facestream.ndjson"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("tracks.json"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("continuity_boxstream.json"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("summary.json")));
    const bool initialRunLooksLegacyOnly =
        !initialRunHasArtifactState &&
        !initialRunDir.entryList(QStringList{QStringLiteral("*_continuity_boxstream_request.json")},
                                 QDir::Files).isEmpty();
    if (initialRunLooksLegacyOnly) {
        debugRun = speaker_flow_debug::createNewRunFrom(debugRun);
    }
    const QString artifactDir = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("facestream_artifact"));
    QDir().mkpath(artifactDir);
    const QString requestPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facestream_request.json").arg(debugRun.videoStem));
    const QString logPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facestream_generator_log.txt").arg(debugRun.videoStem));
    const QString outputPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("continuity_boxstream.json"));
    const QString tracksPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("tracks.json"));
    const QString summaryPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("summary.json"));
    const QString ndjsonPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("facestream.ndjson"));
    const QString indexPath = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("index.json"));

    DetectorRuntimeSettings detectorSettings;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(mediaPath);
    loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, nullptr);

    const int64_t startFrame = qMax<int64_t>(0, selectedClip->sourceInFrame);
    const int64_t maxFrames = qMax<int64_t>(0, selectedClip->durationFrames);
    QStringList args{
        mediaPath,
        QStringLiteral("--detector"), QStringLiteral("scrfd-ncnn-vulkan"),
        QStringLiteral("--require-zero-copy"),
        QStringLiteral("--stride"), QString::number(qMax(1, detectorSettings.stride)),
        QStringLiteral("--start-frame"), QString::number(startFrame),
        QStringLiteral("--no-preview-window"),
        QStringLiteral("--no-preview-files"),
        QStringLiteral("--quiet"),
        QStringLiteral("--progress"),
        QStringLiteral("--out-dir"), artifactDir
    };
    if (maxFrames > 0) {
        args << QStringLiteral("--max-frames") << QString::number(maxFrames);
    }

    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("engine")] = QStringLiteral("jcut_vulkan_boxstream_offscreen_scrfd_zero_copy_v1");
    request[QStringLiteral("generator_binary")] = generatorPath;
    request[QStringLiteral("media_path")] = mediaPath;
    request[QStringLiteral("clip_id")] = selectedClip->id;
    request[QStringLiteral("source_start_frame")] = static_cast<qint64>(startFrame);
    request[QStringLiteral("max_frames")] = static_cast<qint64>(maxFrames);
    request[QStringLiteral("detector_settings_file")] = detectorSettingsPath;
    request[QStringLiteral("detector_settings")] = detectorRuntimeSettingsToJson(detectorSettings, QStringLiteral("scrfd-ncnn-vulkan"), 640);
    request[QStringLiteral("artifact_out_dir")] = artifactDir;
    request[QStringLiteral("facestream_ndjson")] = ndjsonPath;
    request[QStringLiteral("tracks_json")] = tracksPath;
    request[QStringLiteral("continuity_boxstream_json")] = outputPath;
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
    progressDialog.resize(760, 420);
    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);
    auto* statusLabel = new QLabel(QStringLiteral("Generating resumable SCRFD zero-copy FaceStream artifact..."), &progressDialog);
    statusLabel->setWordWrap(true);
    progressLayout->addWidget(statusLabel);
    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, 0);
    progressLayout->addWidget(progressBar);
    auto* logEdit = new QPlainTextEdit(&progressDialog);
    logEdit->setReadOnly(true);
    logEdit->setMaximumBlockCount(400);
    progressLayout->addWidget(logEdit, 1);
    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch(1);
    auto* cancelRunButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    progressButtons->addWidget(cancelRunButton);
    progressLayout->addLayout(progressButtons);

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(QFileInfo(generatorPath).absolutePath());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    bool canceled = false;
    connect(cancelRunButton, &QPushButton::clicked, [&]() {
        canceled = true;
        statusLabel->setText(QStringLiteral("Canceling after current child-process checkpoint flush..."));
        process.terminate();
    });

    QFile logFile(logPath);
    logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    process.start(generatorPath, args);
    if (!process.waitForStarted(3000)) {
        const QString message = QStringLiteral("Failed to start FaceStream generator: %1").arg(process.errorString());
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            message, {requestPath, logPath, ndjsonPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), message);
        return;
    }
    progressDialog.show();
    QString stdoutRemainder;
    QString stderrRemainder;
    while (!process.waitForFinished(100)) {
        QApplication::processEvents();
        const QByteArray out = process.readAllStandardOutput();
        const QByteArray err = process.readAllStandardError();
        if (!out.isEmpty()) {
            if (logFile.isOpen()) {
                logFile.write(out);
                logFile.flush();
            }
            stdoutRemainder = appendProcessText(logEdit, stdoutRemainder, out);
        }
        if (!err.isEmpty()) {
            if (logFile.isOpen()) {
                logFile.write(err);
                logFile.flush();
            }
            stderrRemainder = appendProcessText(logEdit, stderrRemainder, err);
        }
        if (canceled && process.state() != QProcess::NotRunning && !process.waitForFinished(1200)) {
            process.kill();
        }
    }
    const QByteArray finalOut = process.readAllStandardOutput();
    const QByteArray finalErr = process.readAllStandardError();
    if (!finalOut.isEmpty()) {
        if (logFile.isOpen()) {
            logFile.write(finalOut);
        }
        stdoutRemainder = appendProcessText(logEdit, stdoutRemainder + QLatin1Char('\n'), finalOut);
    }
    if (!finalErr.isEmpty()) {
        if (logFile.isOpen()) {
            logFile.write(finalErr);
        }
        stderrRemainder = appendProcessText(logEdit, stderrRemainder + QLatin1Char('\n'), finalErr);
    }
    if (logFile.isOpen()) {
        logFile.close();
    }
    progressDialog.close();

    const bool processOk = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (!processOk) {
        const QString message = canceled
            ? QStringLiteral("JCut DNN FaceStream Generator canceled. Partial facestream.ndjson remains resumable in the artifact folder.")
            : QStringLiteral("JCut DNN FaceStream Generator failed. See the generator log for details.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            message, {requestPath, logPath, ndjsonPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), message);
        return;
    }

    QString parseError;
    QJsonObject generatedArtifact;
    if (!readJsonObject(outputPath, &generatedArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            parseError, {requestPath, logPath, ndjsonPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), parseError);
        return;
    }

    const QJsonObject generatedByClip = generatedArtifact.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
    QJsonObject continuityRoot = generatedByClip.value(QStringLiteral("boxstream-offscreen-source")).toObject();
    QJsonArray streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) {
        const QString noTracksMessage = QStringLiteral("Generated FaceStream artifact contains no continuity streams for this clip.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            noTracksMessage, {requestPath, logPath, ndjsonPath, tracksPath, outputPath, summaryPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), noTracksMessage);
        return;
    }
    continuityRoot[QStringLiteral("run_id")] = debugRun.runId;
    continuityRoot[QStringLiteral("imported_from_artifact_dir")] = artifactDir;
    continuityRoot[QStringLiteral("facestream_ndjson")] = ndjsonPath;
    continuityRoot[QStringLiteral("summary_json")] = summaryPath;
    continuityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    const QString clipId = selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id.trimmed();
    QJsonObject artifactRoot;
    const bool saved = jcut::boxstream::saveContinuityArtifact(
        m_loadedTranscriptPath,
        clipId,
        continuityRoot,
        &artifactRoot);
    QString statusMessage = saved
        ? QStringLiteral("Imported generated FaceStream artifact.")
        : QStringLiteral("Generated FaceStream artifact, but failed to save transcript artifact.");
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), saved ? QStringLiteral("ok") : QStringLiteral("error"),
        statusMessage, {requestPath, logPath, ndjsonPath, tracksPath, outputPath, summaryPath});

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
