#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "transcript_engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>

namespace {
struct AiProposalRow {
    QString targetId;
    QString field;
    QString currentValue;
    QString proposedValue;
    qreal confidence = 0.0;
    QString rationale;
};

bool confirmAiProposals(QWidget* parent,
                        const QString& title,
                        const QString& summary,
                        const QVector<AiProposalRow>& proposals)
{
    if (proposals.isEmpty()) {
        return false;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.resize(860, 460);
    auto* rootLayout = new QVBoxLayout(&dialog);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);
    auto* intro = new QLabel(summary, &dialog);
    intro->setWordWrap(true);
    rootLayout->addWidget(intro);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        QStringList{QStringLiteral("Target"),
                    QStringLiteral("Field"),
                    QStringLiteral("Current"),
                    QStringLiteral("Proposed"),
                    QStringLiteral("Confidence"),
                    QStringLiteral("Rationale")});
    table->setRowCount(proposals.size());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    for (int i = 0; i < proposals.size(); ++i) {
        const AiProposalRow& p = proposals[i];
        table->setItem(i, 0, new QTableWidgetItem(p.targetId));
        table->setItem(i, 1, new QTableWidgetItem(p.field));
        table->setItem(i, 2, new QTableWidgetItem(p.currentValue));
        table->setItem(i, 3, new QTableWidgetItem(p.proposedValue));
        table->setItem(i, 4, new QTableWidgetItem(QStringLiteral("%1%").arg(qRound(p.confidence * 100.0))));
        table->setItem(i, 5, new QTableWidgetItem(p.rationale));
    }
    rootLayout->addWidget(table, 1);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyButton = new QPushButton(QStringLiteral("Apply"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(applyButton);
    rootLayout->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace

void SpeakersTab::onSpeakerEnableTrackingClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!setSpeakerTrackingEnabled(speakerId, true)) {
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
    refresh();
}

void SpeakersTab::onSpeakerDisableTrackingClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!setSpeakerTrackingEnabled(speakerId, false)) {
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
    refresh();
}

void SpeakersTab::onSpeakerDeletePointstreamClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!deleteSpeakerAutoTrackPointstream(speakerId)) {
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
    refresh();
}

void SpeakersTab::onSpeakerPrecropFacesClicked()
{
    if (!activeCutMutable() || !m_loadedTranscriptDoc.isObject() || m_loadedTranscriptPath.isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }

    const QString pythonPath =
        QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()
            ? QStandardPaths::findExecutable(QStringLiteral("python"))
            : QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (pythonPath.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("Python was not found in PATH (python3/python)."));
        return;
    }

    const QString scriptPath =
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("speaker_face_candidates.py"));
    if (!QFileInfo::exists(scriptPath)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("speaker_face_candidates.py was not found in the project root."));
        return;
    }

    auto resolveMediaPath = [&](const TimelineClip& currentClip) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
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
        const QString sourcePath = currentClip.filePath.trimmed();
        const QFileInfo sourceInfo(sourcePath);
        if (!sourcePath.isEmpty() &&
            sourceInfo.exists() &&
            (sourceInfo.isFile() || isImageSequencePath(sourcePath))) {
            return sourcePath;
        }
        return QString();
    };

    const QString mediaPath = resolveMediaPath(*clip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("No playable media was found for this clip."));
        return;
    }

    const auto sanitizedToken = [](const QString& raw) {
        QString token = raw;
        token = token.trimmed();
        if (token.isEmpty()) {
            return QStringLiteral("unknown");
        }
        for (QChar& ch : token) {
            const bool ok =
                (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
                (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
                (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                ch == QLatin1Char('.') ||
                ch == QLatin1Char('_') ||
                ch == QLatin1Char('-');
            if (!ok) {
                ch = QLatin1Char('_');
            }
        }
        while (token.contains(QStringLiteral("__"))) {
            token.replace(QStringLiteral("__"), QStringLiteral("_"));
        }
        token = token.left(96);
        return token.isEmpty() ? QStringLiteral("unknown") : token;
    };

    struct DebugRun {
        QString projectId;
        QString clipId;
        QString videoStem;
        QString runId;
        QString runDir;
        QString indexPath;
        QString overwriteDecisionPath;
        QJsonObject stageStatus;
        QJsonArray artefacts;
    };

    const auto nowRunId = []() {
        return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
               QStringLiteral("-") +
               QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    };

    const auto deriveProjectId = [&](const QString& transcriptPath) {
        const QRegularExpression re(QStringLiteral(".*/projects/([^/]+)/.*"));
        const QRegularExpressionMatch m = re.match(transcriptPath);
        if (!m.hasMatch()) {
            return QString();
        }
        return m.captured(1).trimmed();
    };

    const QString projectId = sanitizedToken(deriveProjectId(m_loadedTranscriptPath));
    const QString clipId = sanitizedToken(clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id);
    const QString videoStem = sanitizedToken(QFileInfo(mediaPath).completeBaseName());

    const auto makeDebugRun = [&](const QString& requestedRunId) -> DebugRun {
        DebugRun run;
        run.projectId = projectId;
        run.clipId = clipId;
        run.videoStem = videoStem;
        run.runId = requestedRunId.isEmpty() ? nowRunId() : requestedRunId;

        QString baseRoot;
        const QRegularExpression re(QStringLiteral("(.*/projects/[^/]+)/.*"));
        const QRegularExpressionMatch m = re.match(m_loadedTranscriptPath);
        if (m.hasMatch()) {
            baseRoot = m.captured(1);
        } else {
            baseRoot = QDir::currentPath();
        }
        const QString debugRoot = QDir(baseRoot).absoluteFilePath(
            QStringLiteral("debug/speaker_flow/%1").arg(run.clipId));
        run.runDir = QDir(debugRoot).absoluteFilePath(run.runId);
        QDir().mkpath(run.runDir);
        run.indexPath = QDir(run.runDir).absoluteFilePath(
            QStringLiteral("%1_index.json").arg(run.videoStem));
        run.overwriteDecisionPath = QDir(run.runDir).absoluteFilePath(
            QStringLiteral("%1_overwrite_decision.json").arg(run.videoStem));
        return run;
    };

    DebugRun debugRun = makeDebugRun(QString());

    const auto addArtefact = [&](DebugRun& run, const QString& absolutePath) {
        const QString relative = QDir(run.runDir).relativeFilePath(absolutePath);
        run.artefacts.push_back(relative);
    };
    const auto setStageStatus = [&](DebugRun& run, const QString& stage, const QString& status, const QString& message) {
        QJsonObject statusObj;
        statusObj[QStringLiteral("status")] = status;
        statusObj[QStringLiteral("message")] = message;
        statusObj[QStringLiteral("updated_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        run.stageStatus[stage] = statusObj;
    };
    const auto persistIndex = [&](const DebugRun& run) {
        QJsonObject root;
        root[QStringLiteral("schema_version")] = QStringLiteral("1.0");
        root[QStringLiteral("run_id")] = run.runId;
        root[QStringLiteral("project_id")] = run.projectId;
        root[QStringLiteral("clip_id")] = run.clipId;
        root[QStringLiteral("video_filename")] = QFileInfo(mediaPath).fileName();
        root[QStringLiteral("transcript_path")] = m_loadedTranscriptPath;
        root[QStringLiteral("completed_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        root[QStringLiteral("stage_status")] = run.stageStatus;
        root[QStringLiteral("artefacts")] = run.artefacts;
        QFile file(run.indexPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            file.close();
        }
    };
    const auto recordOverwriteDecision = [&](const DebugRun& run,
                                             const QString& stage,
                                             const QStringList& files,
                                             bool approved) {
        QJsonObject root;
        QFile f(run.overwriteDecisionPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                root = doc.object();
            }
            f.close();
        }
        QJsonArray decisions = root.value(QStringLiteral("decisions")).toArray();
        QJsonObject decision;
        decision[QStringLiteral("stage")] = stage;
        decision[QStringLiteral("approved_by_user")] = approved;
        decision[QStringLiteral("timestamp_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QJsonArray fileArray;
        for (const QString& file : files) {
            fileArray.push_back(QDir(run.runDir).relativeFilePath(file));
        }
        decision[QStringLiteral("files")] = fileArray;
        decisions.push_back(decision);
        root[QStringLiteral("decisions")] = decisions;
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            f.close();
        }
    };

    const auto ensureWritableArtefacts = [&](const QString& stage,
                                             const QStringList& files,
                                             bool allowCreateNewRun,
                                             bool* createNewRunOut) -> bool {
        if (createNewRunOut) {
            *createNewRunOut = false;
        }
        QStringList existing;
        for (const QString& file : files) {
            if (QFileInfo::exists(file)) {
                existing.push_back(file);
            }
        }
        if (existing.isEmpty()) {
            return true;
        }

        QMessageBox dialog;
        dialog.setIcon(QMessageBox::Warning);
        dialog.setWindowTitle(QStringLiteral("Overwrite Debug Artefacts"));
        QString details;
        const int previewCount = qMin(10, existing.size());
        for (int i = 0; i < previewCount; ++i) {
            details += QStringLiteral("- %1\n").arg(QDir(debugRun.runDir).relativeFilePath(existing.at(i)));
        }
        if (existing.size() > previewCount) {
            details += QStringLiteral("- ... and %1 more\n").arg(existing.size() - previewCount);
        }
        dialog.setText(
            QStringLiteral("Stage \"%1\" will overwrite %2 artefact(s).")
                .arg(stage)
                .arg(existing.size()));
        dialog.setInformativeText(details.trimmed());
        QPushButton* overwriteButton =
            dialog.addButton(QStringLiteral("Overwrite"), QMessageBox::AcceptRole);
        QPushButton* cancelButton =
            dialog.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        QPushButton* newRunButton = nullptr;
        if (allowCreateNewRun) {
            newRunButton = dialog.addButton(QStringLiteral("Create New Run Instead"), QMessageBox::ActionRole);
            dialog.setDefaultButton(newRunButton);
        } else {
            dialog.setDefaultButton(overwriteButton);
        }
        dialog.exec();

        if (dialog.clickedButton() == cancelButton) {
            recordOverwriteDecision(debugRun, stage, existing, false);
            return false;
        }
        if (newRunButton && dialog.clickedButton() == newRunButton) {
            if (createNewRunOut) {
                *createNewRunOut = true;
            }
            return false;
        }
        if (dialog.clickedButton() != overwriteButton) {
            recordOverwriteDecision(debugRun, stage, existing, false);
            return false;
        }

        recordOverwriteDecision(debugRun, stage, existing, true);
        for (const QString& file : existing) {
            QFileInfo info(file);
            if (info.isDir()) {
                QDir(file).removeRecursively();
            } else {
                QFile::remove(file);
            }
        }
        return true;
    };

    const int64_t startFrame = qMax<int64_t>(0, clip->sourceInFrame);
    const int64_t endFrame = qMax<int64_t>(startFrame, clip->sourceInFrame + qMax<int64_t>(0, clip->durationFrames - 1));
    const double sourceFps = clip->sourceFps > 0.0 ? clip->sourceFps : 30.0;
    const int stepFrames = 45;
    const int maxCandidates = 24;

    QString cropsDir;
    QString outputJsonPath;
    QString requestPath;
    QString logPath;
    while (true) {
        cropsDir = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_face_crops").arg(debugRun.videoStem));
        outputJsonPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_face_detection_output.json").arg(debugRun.videoStem));
        requestPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_face_detection_request.json").arg(debugRun.videoStem));
        logPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_face_detection_log.txt").arg(debugRun.videoStem));

        bool createNewRun = false;
        if (!ensureWritableArtefacts(
                QStringLiteral("stage_3_face_detection"),
                QStringList{requestPath, outputJsonPath, logPath, cropsDir},
                true,
                &createNewRun)) {
            if (createNewRun) {
                debugRun = makeDebugRun(QString());
                continue;
            }
            setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("skipped"),
                           QStringLiteral("Canceled due to overwrite prompt."));
            persistIndex(debugRun);
            return;
        }
        break;
    }

    QDir().mkpath(cropsDir);
    {
        QJsonObject request;
        request[QStringLiteral("video")] = mediaPath;
        request[QStringLiteral("start_frame")] = static_cast<qint64>(startFrame);
        request[QStringLiteral("end_frame")] = static_cast<qint64>(endFrame);
        request[QStringLiteral("step")] = stepFrames;
        request[QStringLiteral("source_fps")] = sourceFps;
        request[QStringLiteral("max_candidates")] = maxCandidates;
        request[QStringLiteral("run_id")] = debugRun.runId;
        QFile f(requestPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(request).toJson(QJsonDocument::Indented));
            f.close();
        }
    }

    QStringList args;
    args << scriptPath
         << QStringLiteral("--video") << mediaPath
         << QStringLiteral("--output-json") << outputJsonPath
         << QStringLiteral("--output-dir") << cropsDir
         << QStringLiteral("--crop-prefix") << QStringLiteral("%1_face_crop").arg(debugRun.videoStem)
         << QStringLiteral("--start-frame") << QString::number(startFrame)
         << QStringLiteral("--end-frame") << QString::number(endFrame)
         << QStringLiteral("--step") << QString::number(stepFrames)
         << QStringLiteral("--source-fps") << QString::number(sourceFps, 'f', 6)
         << QStringLiteral("--max-candidates") << QString::number(maxCandidates);

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(pythonPath, args);
    if (!process.waitForStarted(5000)) {
        setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("error"),
                       QStringLiteral("Failed to start face candidate detector process."));
        persistIndex(debugRun);
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("Failed to start face candidate detector."));
        return;
    }

    QProgressDialog progressDialog(
        QStringLiteral("Scanning frames for potential faces..."),
        QStringLiteral("Cancel"),
        0,
        0,
        nullptr);
    progressDialog.setWindowTitle(QStringLiteral("Pre-crop Faces"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumDuration(0);
    progressDialog.setAutoClose(false);
    progressDialog.setAutoReset(false);
    progressDialog.show();

    QString processLog;
    bool canceled = false;
    bool finished = false;
    while (!finished) {
        finished = process.waitForFinished(120);
        const QString liveOutput = QString::fromUtf8(process.readAllStandardOutput());
        if (!liveOutput.isEmpty()) {
            processLog += liveOutput;
            const QStringList lines = liveOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (!lines.isEmpty()) {
                progressDialog.setLabelText(
                    QStringLiteral("Scanning frames for potential faces...\n%1")
                        .arg(lines.constLast().left(140)));
            }
        }
        QApplication::processEvents();
        if (progressDialog.wasCanceled()) {
            canceled = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
    }
    progressDialog.close();

    const QString tailOutput = QString::fromUtf8(process.readAllStandardOutput());
    if (!tailOutput.isEmpty()) {
        processLog += tailOutput;
    }
    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logFile.write(processLog.toUtf8());
        logFile.close();
    }

    if (canceled) {
        setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("skipped"),
                       QStringLiteral("User canceled face candidate scan."));
        addArtefact(debugRun, requestPath);
        addArtefact(debugRun, logPath);
        persistIndex(debugRun);
        QMessageBox::information(nullptr,
                                 QStringLiteral("Pre-crop Faces"),
                                 QStringLiteral("Face candidate scan canceled."));
        return;
    }

    const QString processOutput = processLog.trimmed();
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("error"),
                       QStringLiteral("Face candidate detection script failed."));
        addArtefact(debugRun, requestPath);
        addArtefact(debugRun, logPath);
        persistIndex(debugRun);
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("Face candidate detection failed.\n\n%1")
                                 .arg(processOutput.isEmpty() ? QStringLiteral("(no output)") : processOutput));
        return;
    }

    QFile outputFile(outputJsonPath);
    if (!outputFile.open(QIODevice::ReadOnly)) {
        setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("error"),
                       QStringLiteral("Detector did not produce output JSON."));
        addArtefact(debugRun, requestPath);
        addArtefact(debugRun, logPath);
        persistIndex(debugRun);
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("Detector did not produce candidate output JSON."));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument outputDoc = QJsonDocument::fromJson(outputFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !outputDoc.isObject()) {
        setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("error"),
                       QStringLiteral("Invalid face candidate output JSON."));
        addArtefact(debugRun, requestPath);
        addArtefact(debugRun, outputJsonPath);
        addArtefact(debugRun, logPath);
        persistIndex(debugRun);
        QMessageBox::warning(nullptr,
                             QStringLiteral("Pre-crop Faces"),
                             QStringLiteral("Invalid candidate output JSON."));
        return;
    }
    addArtefact(debugRun, requestPath);
    addArtefact(debugRun, outputJsonPath);
    addArtefact(debugRun, logPath);
    addArtefact(debugRun, cropsDir);
    setStageStatus(debugRun, QStringLiteral("stage_3_face_detection"), QStringLiteral("ok"),
                   QStringLiteral("Face candidate detection completed."));
    persistIndex(debugRun);

    struct FaceCandidate {
        int64_t frame = 0;
        qreal x = 0.5;
        qreal y = 0.5;
        qreal box = 0.20;
        qreal score = 0.0;
        QString cropPath;
    };
    QVector<FaceCandidate> candidates;
    const QJsonArray candidateArray = outputDoc.object().value(QStringLiteral("candidates")).toArray();
    candidates.reserve(candidateArray.size());
    for (const QJsonValue& value : candidateArray) {
        const QJsonObject obj = value.toObject();
        FaceCandidate candidate;
        candidate.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
        candidate.x = qBound<qreal>(0.0, obj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
        candidate.y = qBound<qreal>(0.0, obj.value(QStringLiteral("y")).toDouble(0.5), 1.0);
        candidate.box = qBound<qreal>(0.01, obj.value(QStringLiteral("box")).toDouble(0.20), 1.0);
        candidate.score = qBound<qreal>(0.0, obj.value(QStringLiteral("score")).toDouble(0.0), 1.0);
        candidate.cropPath = obj.value(QStringLiteral("crop_path")).toString();
        if (!candidate.cropPath.trimmed().isEmpty()) {
            QFileInfo cropInfo(candidate.cropPath);
            candidate.cropPath = cropInfo.isRelative()
                ? QDir(cropsDir).absoluteFilePath(candidate.cropPath)
                : cropInfo.absoluteFilePath();
        }
        candidates.push_back(candidate);
    }
    if (candidates.isEmpty()) {
        setStageStatus(debugRun, QStringLiteral("stage_4_assignment"), QStringLiteral("warn"),
                       QStringLiteral("No candidates produced."));
        persistIndex(debugRun);
        QMessageBox::information(nullptr,
                                 QStringLiteral("Pre-crop Faces"),
                                 QStringLiteral("No face candidates were found on sampled frames."));
        return;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QStringList speakerIds;
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it) {
        const QString speakerId = it.key().trimmed();
        if (!speakerId.isEmpty()) {
            speakerIds.push_back(speakerId);
        }
    }
    if (speakerIds.isEmpty() && m_widgets.speakersTable) {
        for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
            QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
            if (!idItem) {
                continue;
            }
            const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
            if (!speakerId.isEmpty()) {
                speakerIds.push_back(speakerId);
            }
        }
        speakerIds.removeDuplicates();
    }
    std::sort(speakerIds.begin(), speakerIds.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });
    if (speakerIds.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Pre-crop Faces"),
                                 QStringLiteral("No transcript speaker IDs were available to assign candidates."));
        return;
    }

    auto suggestedSpeakerForFrame = [&](int64_t frame30) -> QString {
        const double timeSeconds = static_cast<double>(qMax<int64_t>(0, frame30)) / 30.0;
        const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
        QString nearestSpeaker;
        double nearestDistance = std::numeric_limits<double>::max();
        for (const QJsonValue& segValue : segments) {
            const QJsonObject segObj = segValue.toObject();
            const QString segmentSpeaker =
                segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
            const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    continue;
                }
                QString speaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (speaker.isEmpty()) {
                    speaker = segmentSpeaker;
                }
                if (speaker.isEmpty()) {
                    continue;
                }
                const double start = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double end = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                if (start < 0.0 || end < 0.0) {
                    continue;
                }
                if (timeSeconds >= start && timeSeconds <= end) {
                    return speaker;
                }
                const double distance = qMin(std::abs(timeSeconds - start), std::abs(timeSeconds - end));
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestSpeaker = speaker;
                }
            }
        }
        return nearestSpeaker;
    };

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Assign Pre-cropped Face Candidates"));
    dialog.resize(920, 560);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* intro = new QLabel(
        QStringLiteral("Review sampled face crops, then assign each crop to a transcript speaker. "
                       "Auto suggestions are preselected from transcript timing; manual speaker ID override is optional. "
                       "On apply, assignments fill only empty Ref1/Ref2 slots."),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(7);
    table->setHorizontalHeaderLabels(
        QStringList{
            QStringLiteral("Crop"),
            QStringLiteral("Frame"),
            QStringLiteral("X"),
            QStringLiteral("Y"),
            QStringLiteral("Box"),
            QStringLiteral("Assign To (Auto)"),
            QStringLiteral("Manual Speaker ID (Override)")});
    table->setRowCount(candidates.size());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);

    auto* allowNewSpeakerIdsCheck = new QCheckBox(
        QStringLiteral("Allow creating new speaker IDs from manual override"),
        &dialog);
    allowNewSpeakerIdsCheck->setChecked(false);
    allowNewSpeakerIdsCheck->setToolTip(
        QStringLiteral("Disabled by default to prevent typo-based speaker ID creation."));

    auto makeSpeakerLabel = [&](const QString& speakerId) {
        const QString name =
            profiles.value(speakerId).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
        return name.isEmpty() || name == speakerId
            ? speakerId
            : QStringLiteral("%1 (%2)").arg(speakerId, name);
    };

    for (int row = 0; row < candidates.size(); ++row) {
        const FaceCandidate& candidate = candidates.at(row);
        auto* cropItem = new QTableWidgetItem;
        cropItem->setFlags(cropItem->flags() & ~Qt::ItemIsEditable);
        cropItem->setTextAlignment(Qt::AlignCenter);
        if (!candidate.cropPath.isEmpty()) {
            QPixmap crop(candidate.cropPath);
            if (!crop.isNull()) {
                cropItem->setIcon(QIcon(crop.scaled(84, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
        }
        cropItem->setToolTip(QStringLiteral("score=%1").arg(QString::number(candidate.score, 'f', 3)));
        cropItem->setSizeHint(QSize(88, 88));
        table->setItem(row, 0, cropItem);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(candidate.frame)));
        table->setItem(row, 2, new QTableWidgetItem(QString::number(candidate.x, 'f', 3)));
        table->setItem(row, 3, new QTableWidgetItem(QString::number(candidate.y, 'f', 3)));
        table->setItem(row, 4, new QTableWidgetItem(QString::number(candidate.box, 'f', 3)));

        auto* assignCombo = new QComboBox(table);
        assignCombo->addItem(QStringLiteral("Skip"), QString());
        for (const QString& speakerId : speakerIds) {
            assignCombo->addItem(makeSpeakerLabel(speakerId), speakerId);
        }
        table->setCellWidget(row, 5, assignCombo);

        const QString suggestedSpeakerId = suggestedSpeakerForFrame(candidate.frame);
        if (!suggestedSpeakerId.isEmpty()) {
            const int suggestedIndex = assignCombo->findData(suggestedSpeakerId);
            if (suggestedIndex >= 0) {
                assignCombo->setCurrentIndex(suggestedIndex);
            }
        }

        auto* manualIdEdit = new QLineEdit(table);
        manualIdEdit->setPlaceholderText(QStringLiteral("Optional: type speaker ID"));
        table->setCellWidget(row, 6, manualIdEdit);
        table->setRowHeight(row, 92);
    }
    layout->addWidget(table, 1);
    layout->addWidget(allowNewSpeakerIdsCheck);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyButton = new QPushButton(QStringLiteral("Apply Assignments"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(applyButton);
    layout->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    QHash<QString, QString> speakerIdByLower;
    speakerIdByLower.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        speakerIdByLower.insert(speakerId.toLower(), speakerId);
    }

    QHash<QString, QVector<FaceCandidate>> assignmentsBySpeaker;
    QJsonArray assignmentTableRows;
    const QRegularExpression speakerIdPattern(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    while (true) {
        if (dialog.exec() != QDialog::Accepted) {
            setStageStatus(debugRun, QStringLiteral("stage_4_assignment"), QStringLiteral("skipped"),
                           QStringLiteral("User canceled assignment dialog."));
            persistIndex(debugRun);
            return;
        }

        assignmentsBySpeaker.clear();
        assignmentTableRows = QJsonArray();
        QStringList invalidIds;
        QStringList unknownIds;
        for (int row = 0; row < table->rowCount(); ++row) {
            auto* combo = qobject_cast<QComboBox*>(table->cellWidget(row, 5));
            auto* manualIdEdit = qobject_cast<QLineEdit*>(table->cellWidget(row, 6));
            if (!combo || !manualIdEdit) {
                continue;
            }
            const QString manualSpeakerId = manualIdEdit->text().trimmed();
            QString speakerId =
                manualSpeakerId.isEmpty() ? combo->currentData().toString().trimmed() : manualSpeakerId;
            const FaceCandidate& c = candidates.at(row);
            QJsonObject assignmentRow{
                {QStringLiteral("row"), row},
                {QStringLiteral("frame"), static_cast<qint64>(c.frame)},
                {QStringLiteral("x"), c.x},
                {QStringLiteral("y"), c.y},
                {QStringLiteral("box"), c.box},
                {QStringLiteral("score"), c.score},
                {QStringLiteral("auto_selected"), combo->currentData().toString().trimmed()},
                {QStringLiteral("manual_override"), manualSpeakerId},
                {QStringLiteral("resolved_speaker_id"), speakerId}
            };
            if (speakerId.isEmpty()) {
                assignmentRow[QStringLiteral("decision")] = QStringLiteral("skipped");
                assignmentTableRows.push_back(assignmentRow);
                continue;
            }

            if (!speakerIdPattern.match(speakerId).hasMatch()) {
                if (!invalidIds.contains(speakerId)) {
                    invalidIds.push_back(speakerId);
                }
                assignmentRow[QStringLiteral("decision")] = QStringLiteral("invalid_id");
                assignmentTableRows.push_back(assignmentRow);
                continue;
            }

            const QString normalizedExistingId =
                speakerIdByLower.value(speakerId.toLower(), QString());
            if (!normalizedExistingId.isEmpty()) {
                speakerId = normalizedExistingId;
            } else if (!allowNewSpeakerIdsCheck->isChecked()) {
                if (!unknownIds.contains(speakerId)) {
                    unknownIds.push_back(speakerId);
                }
                assignmentRow[QStringLiteral("decision")] = QStringLiteral("unknown_id_blocked");
                assignmentTableRows.push_back(assignmentRow);
                continue;
            }

            assignmentRow[QStringLiteral("resolved_speaker_id")] = speakerId;
            assignmentRow[QStringLiteral("decision")] = QStringLiteral("accepted");
            assignmentTableRows.push_back(assignmentRow);
            assignmentsBySpeaker[speakerId].push_back(candidates.at(row));
        }

        if (!invalidIds.isEmpty()) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Invalid Manual Speaker ID"),
                QStringLiteral("Manual speaker IDs must match `[A-Za-z0-9_.-]` and be 1-64 chars.\n\nInvalid: %1")
                    .arg(invalidIds.join(QStringLiteral(", "))));
            continue;
        }
        if (!unknownIds.isEmpty()) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Unknown Speaker ID"),
                QStringLiteral("Manual speaker IDs must match existing transcript speakers unless "
                               "\"Allow creating new speaker IDs\" is enabled.\n\nUnknown: %1")
                    .arg(unknownIds.join(QStringLiteral(", "))));
            continue;
        }
        break;
    }

    if (assignmentsBySpeaker.isEmpty()) {
        setStageStatus(debugRun, QStringLiteral("stage_4_assignment"), QStringLiteral("warn"),
                       QStringLiteral("No assignments accepted."));
        persistIndex(debugRun);
        return;
    }

    const QString assignmentTablePath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_assignment_table.json").arg(debugRun.videoStem));
    const QString assignmentDecisionsPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_assignment_decisions.json").arg(debugRun.videoStem));
    bool unusedCreateNewRun = false;
    if (!ensureWritableArtefacts(
            QStringLiteral("stage_4_assignment"),
            QStringList{assignmentTablePath, assignmentDecisionsPath},
            false,
            &unusedCreateNewRun)) {
        setStageStatus(debugRun, QStringLiteral("stage_4_assignment"), QStringLiteral("skipped"),
                       QStringLiteral("Canceled due to overwrite prompt."));
        persistIndex(debugRun);
        return;
    }
    {
        QJsonObject assignmentTableRoot;
        assignmentTableRoot[QStringLiteral("run_id")] = debugRun.runId;
        assignmentTableRoot[QStringLiteral("rows")] = assignmentTableRows;
        QFile f(assignmentTablePath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(assignmentTableRoot).toJson(QJsonDocument::Indented));
            f.close();
        }
    }
    {
        QJsonObject decisionsRoot;
        decisionsRoot[QStringLiteral("run_id")] = debugRun.runId;
        QJsonArray decisions;
        for (auto it = assignmentsBySpeaker.constBegin(); it != assignmentsBySpeaker.constEnd(); ++it) {
            QJsonObject d;
            d[QStringLiteral("speaker_id")] = it.key();
            QJsonArray assigned;
            for (const FaceCandidate& c : it.value()) {
                QJsonObject row;
                row[QStringLiteral("frame")] = static_cast<qint64>(c.frame);
                row[QStringLiteral("x")] = c.x;
                row[QStringLiteral("y")] = c.y;
                row[QStringLiteral("box")] = c.box;
                row[QStringLiteral("score")] = c.score;
                assigned.push_back(row);
            }
            d[QStringLiteral("candidates")] = assigned;
            decisions.push_back(d);
        }
        decisionsRoot[QStringLiteral("assignments")] = decisions;
        QFile f(assignmentDecisionsPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(decisionsRoot).toJson(QJsonDocument::Indented));
            f.close();
        }
    }
    addArtefact(debugRun, assignmentTablePath);
    addArtefact(debugRun, assignmentDecisionsPath);
    setStageStatus(debugRun, QStringLiteral("stage_4_assignment"), QStringLiteral("ok"),
                   QStringLiteral("Candidate assignment completed."));
    persistIndex(debugRun);

    int savedReferenceCount = 0;
    int blockedSpeakers = 0;
    QJsonArray referenceWritePlan;
    QJsonArray referenceWriteResult;
    for (auto it = assignmentsBySpeaker.begin(); it != assignmentsBySpeaker.end(); ++it) {
        const QString speakerId = it.key();
        QVector<FaceCandidate> assigned = it.value();
        std::sort(assigned.begin(), assigned.end(), [](const FaceCandidate& a, const FaceCandidate& b) {
            if (!qFuzzyCompare(a.score + 1.0, b.score + 1.0)) {
                return a.score > b.score;
            }
            return a.frame < b.frame;
        });

        const QJsonObject currentProfiles =
            m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
        const QJsonObject currentProfile = currentProfiles.value(speakerId).toObject();
        const QJsonObject tracking = speakerFramingObject(currentProfile);
        bool hasRef1 = false;
        bool hasRef2 = false;
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
        QVector<int> freeSlots;
        if (!hasRef1) {
            freeSlots.push_back(1);
        }
        if (!hasRef2) {
            freeSlots.push_back(2);
        }
        QJsonObject planRow;
        planRow[QStringLiteral("speaker_id")] = speakerId;
        QJsonArray freeSlotsJson;
        for (int slot : freeSlots) {
            freeSlotsJson.push_back(slot);
        }
        planRow[QStringLiteral("free_slots")] = freeSlotsJson;
        planRow[QStringLiteral("candidate_count")] = assigned.size();
        referenceWritePlan.push_back(planRow);

        if (freeSlots.isEmpty()) {
            ++blockedSpeakers;
            QJsonObject resultRow;
            resultRow[QStringLiteral("speaker_id")] = speakerId;
            resultRow[QStringLiteral("status")] = QStringLiteral("blocked");
            resultRow[QStringLiteral("reason")] = QStringLiteral("both_refs_already_set");
            referenceWriteResult.push_back(resultRow);
            continue;
        }

        const int assignCount = qMin(freeSlots.size(), assigned.size());
        for (int i = 0; i < assignCount; ++i) {
            const FaceCandidate& candidate = assigned.at(i);
            const bool ok = saveSpeakerTrackingReferenceAt(
                    speakerId,
                    freeSlots.at(i),
                    candidate.frame,
                    candidate.x,
                    candidate.y,
                    candidate.box);
            QJsonObject resultRow;
            resultRow[QStringLiteral("speaker_id")] = speakerId;
            resultRow[QStringLiteral("slot")] = freeSlots.at(i);
            resultRow[QStringLiteral("frame")] = static_cast<qint64>(candidate.frame);
            resultRow[QStringLiteral("status")] = ok ? QStringLiteral("ok") : QStringLiteral("error");
            referenceWriteResult.push_back(resultRow);
            if (ok) {
                ++savedReferenceCount;
            }
        }
    }

    const QString referenceWritePlanPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_reference_write_plan.json").arg(debugRun.videoStem));
    const QString referenceWriteResultPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_reference_write_result.json").arg(debugRun.videoStem));
    bool unusedCreateNewRun2 = false;
    if (!ensureWritableArtefacts(
            QStringLiteral("stage_5_reference_write"),
            QStringList{referenceWritePlanPath, referenceWriteResultPath},
            false,
            &unusedCreateNewRun2)) {
        setStageStatus(debugRun, QStringLiteral("stage_5_reference_write"), QStringLiteral("skipped"),
                       QStringLiteral("Canceled due to overwrite prompt."));
        persistIndex(debugRun);
        return;
    }
    {
        QJsonObject planRoot;
        planRoot[QStringLiteral("run_id")] = debugRun.runId;
        planRoot[QStringLiteral("plan")] = referenceWritePlan;
        QFile f(referenceWritePlanPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(planRoot).toJson(QJsonDocument::Indented));
            f.close();
        }
    }
    {
        QJsonObject resultRoot;
        resultRoot[QStringLiteral("run_id")] = debugRun.runId;
        resultRoot[QStringLiteral("saved_reference_count")] = savedReferenceCount;
        resultRoot[QStringLiteral("blocked_speakers")] = blockedSpeakers;
        resultRoot[QStringLiteral("result")] = referenceWriteResult;
        QFile f(referenceWriteResultPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(resultRoot).toJson(QJsonDocument::Indented));
            f.close();
        }
    }
    addArtefact(debugRun, referenceWritePlanPath);
    addArtefact(debugRun, referenceWriteResultPath);

    if (savedReferenceCount == 0) {
        setStageStatus(debugRun, QStringLiteral("stage_5_reference_write"), QStringLiteral("warn"),
                       QStringLiteral("No references written."));
        persistIndex(debugRun);
        QString message = QStringLiteral("No references were written.");
        if (blockedSpeakers > 0) {
            message += QStringLiteral("\n\nSome speakers already had both Ref1 and Ref2 set.");
        }
        QMessageBox::information(nullptr, QStringLiteral("Pre-crop Faces"), message);
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    setStageStatus(debugRun, QStringLiteral("stage_5_reference_write"), QStringLiteral("ok"),
                   QStringLiteral("Reference write completed."));
    persistIndex(debugRun);
    refresh();
}

