#include "editor.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QProgressDialog>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextCursor>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace editor;

namespace {

QString shellQuote(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(escaped);
}

QString extractJsonObject(const QString& text) {
    const int start = text.indexOf(QLatin1Char('{'));
    const int end = text.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end < start) {
        return QString();
    }
    return text.mid(start, (end - start) + 1);
}

bool parseSyncAction(const QString& value, RenderSyncAction* actionOut) {
    if (!actionOut) {
        return false;
    }
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

struct ShellRunResult {
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QString output;
    QString errorString;
    bool canceled = false;
};

ShellRunResult runShellCommandStreaming(const QString& command,
                                        std::atomic_bool* cancelFlag,
                                        const std::function<void(const QString&)>& logFn,
                                        int timeoutMs = -1) {
    ShellRunResult result;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (logFn) {
        logFn(QStringLiteral("$ %1\n").arg(command));
    }
    process.start(QStringLiteral("/bin/bash"), {QStringLiteral("-lc"), command});
    if (!process.waitForStarted(5000)) {
        result.errorString = QStringLiteral("failed to start shell command");
        return result;
    }

    auto flushOutput = [&]() {
        const QByteArray chunk = process.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            const QString text = QString::fromLocal8Bit(chunk);
            result.output += text;
            if (logFn) {
                logFn(text);
            }
        }
    };

    if (timeoutMs < 0) {
        while (!process.waitForFinished(120)) {
            flushOutput();
            if (cancelFlag && cancelFlag->load()) {
                process.kill();
                process.waitForFinished(2000);
                result.canceled = true;
                break;
            }
        }
        flushOutput();
    } else {
        process.waitForFinished(timeoutMs);
        flushOutput();
    }

    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.errorString = process.errorString();
    return result;
}

} // namespace

void EditorWindow::openTranscriptionWindow(const QString &filePath, const QString &label)
{
    const QFileInfo inputInfo(filePath);
    if (!inputInfo.exists() || !inputInfo.isFile())
    {
        QMessageBox::warning(this,
                             QStringLiteral("Transcribe Failed"),
                             QStringLiteral("The selected file does not exist:\n%1")
                                 .arg(QDir::toNativeSeparators(filePath)));
        return;
    }

    const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("whisperx.sh"));
    if (!QFileInfo::exists(scriptPath))
    {
        QMessageBox::warning(this,
                             QStringLiteral("Transcribe Failed"),
                             QStringLiteral("whisperx.sh was not found at:\n%1")
                                 .arg(QDir::toNativeSeparators(scriptPath)));
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Transcribe  %1").arg(label));
    dialog->resize(920, 560);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("whisperx.sh %1").arg(QDir::toNativeSeparators(filePath)), dialog);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    layout->addWidget(output, 1);

    auto *autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

    auto *inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->setSpacing(8);
    auto *inputLabel = new QLabel(QStringLiteral("stdin"), dialog);
    auto *inputLine = new QLineEdit(dialog);
    inputLine->setPlaceholderText(QStringLiteral("Type input for whisperx.sh prompts, then press Send"));
    auto *sendButton = new QPushButton(QStringLiteral("Send"), dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    inputRow->addWidget(inputLabel);
    inputRow->addWidget(inputLine, 1);
    inputRow->addWidget(sendButton);
    inputRow->addWidget(closeButton);
    layout->addLayout(inputRow);

    auto *process = new QProcess(dialog);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QDir::currentPath());

    const auto appendOutput = [output, autoScrollBox](const QString &text)
    {
        if (text.isEmpty()) return;
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
    };

    connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]()
            { appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput())); });
    connect(process, &QProcess::started, dialog, [appendOutput, filePath]()
            { appendOutput(QStringLiteral("$ ./whisperx.sh \"%1\"\n").arg(QDir::toNativeSeparators(filePath))); });
    connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error)
            { appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error))); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
            [appendOutput](int exitCode, QProcess::ExitStatus exitStatus)
            {
                appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                 .arg(exitCode)
                                 .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed")));
            });
    connect(sendButton, &QPushButton::clicked, dialog, [process, inputLine, appendOutput]()
            {
                const QString text = inputLine->text();
                if (text.isEmpty()) return;
                process->write(text.toUtf8());
                process->write("\n");
                appendOutput(QStringLiteral("> %1\n").arg(text));
                inputLine->clear();
            });
    connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [process](int)
            {
                if (process->state() != QProcess::NotRunning) {
                    process->kill();
                    process->waitForFinished(1000);
                }
            });

    process->start(QStringLiteral("/bin/bash"), {scriptPath, QFileInfo(filePath).absoluteFilePath()});
    dialog->show();
}

QString EditorWindow::defaultProxyOutputPath(const TimelineClip &clip, const MediaProbeResult *knownProbe,
                                              ProxyFormat format) const
{
    const QFileInfo sourceInfo(clip.filePath);
    if (format == ProxyFormat::ImageSequence) {
        // Image sequence proxy: a directory named <basename>.proxy/ containing JPEG frames
        return sourceInfo.dir().absoluteFilePath(
            QStringLiteral("%1.proxy").arg(sourceInfo.completeBaseName()));
    }
    const QString suffix = (format == ProxyFormat::H264) ? QStringLiteral("mp4")
                                                         : QStringLiteral("mov");
    return sourceInfo.dir().absoluteFilePath(
        QStringLiteral("%1.proxy.%2").arg(sourceInfo.completeBaseName(), suffix));
}

QString EditorWindow::clipFileInfoSummary(const QString &filePath, const MediaProbeResult *knownProbe) const
{
    if (filePath.isEmpty()) return QStringLiteral("Path: None");

    const QFileInfo info(filePath);
    QStringList lines;
    lines << QStringLiteral("Path: %1").arg(QDir::toNativeSeparators(filePath));
    lines << QStringLiteral("Exists: %1").arg(info.exists() ? QStringLiteral("Yes") : QStringLiteral("No"));
    if (!info.exists()) return lines.join(QLatin1Char('\n'));

    lines << QStringLiteral("Size: %1 MB").arg(
        QString::number(static_cast<double>(info.size()) / (1024.0 * 1024.0), 'f', 1));
    lines << QStringLiteral("Modified: %1").arg(info.lastModified().toString(Qt::ISODate));
    MediaProbeResult fallbackProbe;
    const MediaProbeResult &probe = knownProbe ? *knownProbe : fallbackProbe;
    lines << QStringLiteral("Media Type: %1").arg(clipMediaTypeLabel(probe.mediaType));
    lines << QStringLiteral("Duration: %1 frames").arg(probe.durationFrames);
    lines << QStringLiteral("Audio: %1").arg(probe.hasAudio ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Video: %1").arg(probe.hasVideo ? QStringLiteral("Yes") : QStringLiteral("No"));
    return lines.join(QLatin1Char('\n'));
}
void EditorWindow::continueProxyForClip(const QString &clipId)
{
    createProxyForClip(clipId, true);
}

void EditorWindow::createProxyForClip(const QString &clipId, bool continueGeneration)
{
    if (!m_timeline) return;

    const TimelineClip *clip = nullptr;
    for (const TimelineClip &candidate : m_timeline->clips())
    {
        if (candidate.id == clipId)
        {
            clip = &candidate;
            break;
        }
    }
    if (!clip) return;
    if (clip->mediaType != ClipMediaType::Video)
    {
        QMessageBox::information(this, QStringLiteral("Create Proxy"),
                                 QStringLiteral("Proxy creation is currently available for video clips."));
        return;
    }

    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpegPath.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                             QStringLiteral("ffmpeg was not found in PATH."));
        return;
    }

    const bool imageSequenceProxy = isImageSequencePath(clip->filePath);
    const QStringList sequenceFrames = imageSequenceProxy ? imageSequenceFramePaths(clip->filePath) : QStringList{};
    if (imageSequenceProxy && sequenceFrames.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                             QStringLiteral("No sequence frames were found for this clip."));
        return;
    }

    const QString probePath = imageSequenceProxy ? sequenceFrames.constFirst() : clip->filePath;
    const MediaProbeResult sourceProbe = probeMediaFile(probePath, clip->durationFrames / kTimelineFps);
    const QString existingProxyPath = playbackProxyPathForClip(*clip);
    const QString outputPath = defaultProxyOutputPath(*clip, &sourceProbe, ProxyFormat::ImageSequence);
    const QString overwriteTarget = !existingProxyPath.isEmpty() ? existingProxyPath : outputPath;
    
    const bool sourceIsVfr = !imageSequenceProxy && isVariableFrameRate(clip->filePath);
    if (sourceIsVfr) {
        const auto response = QMessageBox::question(
            this,
            QStringLiteral("Variable Frame Rate Detected"),
            QStringLiteral("Source video appears variable frame rate (~%1 fps avg).\n\n"
                           "Continue with CFR proxy encoding (recommended)?")
                .arg(sourceProbe.fps, 0, 'f', 2),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (response != QMessageBox::Yes) return;
    }
    
    int resumeNextFrameNumber = 1;
    if (continueGeneration) {
        const QFileInfo proxyInfo(overwriteTarget);
        if (!proxyInfo.exists() || !proxyInfo.isDir()) {
            QMessageBox::information(
                this,
                QStringLiteral("Continue Proxy Gen"),
                QStringLiteral("A generated image-sequence proxy directory was not found for this clip."));
            return;
        }
        const QStringList generatedFrames = QDir(overwriteTarget).entryList(
            {QStringLiteral("frame_*.jpg"), QStringLiteral("frame_*.png")},
            QDir::Files,
            QDir::Name);
        if (generatedFrames.isEmpty()) {
            QMessageBox::information(
                this,
                QStringLiteral("Continue Proxy Gen"),
                QStringLiteral("No generated proxy frames were found. Use Create Proxy instead."));
            return;
        }
        static const QRegularExpression kFrameNumberPattern(QStringLiteral("^frame_(\\d+)\\.(?:jpg|png)$"),
                                                            QRegularExpression::CaseInsensitiveOption);
        int maxFrameNumber = 0;
        for (const QString& fileName : generatedFrames) {
            const QRegularExpressionMatch match = kFrameNumberPattern.match(fileName);
            if (!match.hasMatch()) {
                continue;
            }
            const int parsed = match.captured(1).toInt();
            maxFrameNumber = qMax(maxFrameNumber, parsed);
        }
        resumeNextFrameNumber = qMax(1, maxFrameNumber + 1);
    } else if (QFileInfo::exists(overwriteTarget))
    {
        const auto response = QMessageBox::question(
            this,
            QStringLiteral("Overwrite Proxy"),
            QStringLiteral("A proxy already exists:\n%1\n\nOverwrite it?")
                .arg(QDir::toNativeSeparators(overwriteTarget)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (response != QMessageBox::Yes) return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("%1  %2")
                               .arg(continueGeneration ? QStringLiteral("Continue Proxy Gen")
                                                       : QStringLiteral("Create Proxy"))
                               .arg(clip->label));
    dialog->resize(920, 560);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("ffmpeg proxy for %1").arg(QDir::toNativeSeparators(clip->filePath)), dialog);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *formatRow = new QHBoxLayout;
    formatRow->setContentsMargins(0, 0, 0, 0);
    formatRow->addWidget(new QLabel(QStringLiteral("Format:"), dialog));
    auto *formatCombo = new QComboBox(dialog);
    formatCombo->addItem(QStringLiteral("Image Sequence (JPEG)"), static_cast<int>(ProxyFormat::ImageSequence));
    formatCombo->addItem(QStringLiteral("H.264 (MP4)"), static_cast<int>(ProxyFormat::H264));
#ifndef __APPLE__
    formatCombo->addItem(QStringLiteral("Motion JPEG (MOV)"), static_cast<int>(ProxyFormat::MJPEG));
#endif
    formatCombo->setCurrentIndex(0);
    if (continueGeneration) {
        formatCombo->setCurrentIndex(0);
        formatCombo->setEnabled(false);
    }
    formatRow->addWidget(formatCombo);
    formatRow->addStretch(1);
    layout->addLayout(formatRow);

    auto *output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    layout->addWidget(output, 1);

    auto *autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    auto *process = new QProcess(dialog);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QDir::currentPath());

    std::unique_ptr<QTemporaryFile> sequenceListFile;
    QStringList arguments = {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner")};

    if (imageSequenceProxy)
    {
        sequenceListFile = std::make_unique<QTemporaryFile>(QDir::temp().filePath(QStringLiteral("editor_sequence_proxy_XXXXXX.txt")));
        sequenceListFile->setAutoRemove(false);
        if (!sequenceListFile->open())
        {
            QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                                 QStringLiteral("Unable to create a temporary ffmpeg input list for the image sequence."));
            return;
        }

        QTextStream stream(sequenceListFile.get());
        int sequenceStartIndex = 0;
        if (continueGeneration) {
            sequenceStartIndex = qBound(0, resumeNextFrameNumber - 1, qMax(0, sequenceFrames.size() - 1));
        }

        for (int i = sequenceStartIndex; i < sequenceFrames.size(); ++i)
        {
            const QString& framePath = sequenceFrames[i];
            QString escapedFramePath = QFileInfo(framePath).absoluteFilePath();
            escapedFramePath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
             stream << "file '" << escapedFramePath << "'\n";
             stream << "duration " << QString::number(1.0 / sourceProbe.fps, 'f', 6) << '\n';
        }
        if (sequenceStartIndex < sequenceFrames.size())
        {
            QString escapedFramePath = QFileInfo(sequenceFrames.constLast()).absoluteFilePath();
            escapedFramePath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            stream << "file '" << escapedFramePath << "'\n";
        }
        stream.flush();
        sequenceListFile->flush();

        arguments << QStringLiteral("-f") << QStringLiteral("concat")
                  << QStringLiteral("-safe") << QStringLiteral("0")
                  << QStringLiteral("-i") << sequenceListFile->fileName();
    }
    else
    {
        const auto selectedFormat = static_cast<ProxyFormat>(
            formatCombo->currentData().toInt());

        if (continueGeneration) {
            const qreal seekSeconds = qMax<qreal>(0.0, static_cast<qreal>(resumeNextFrameNumber - 1) /
                                                           static_cast<qreal>(qMax(1, qRound(sourceProbe.fps))));
            arguments << QStringLiteral("-ss")
                      << QString::number(static_cast<double>(seekSeconds), 'f', 6);
        }

        arguments << QStringLiteral("-i") << QFileInfo(clip->filePath).absoluteFilePath()
                  << QStringLiteral("-map") << QStringLiteral("0:v:0");

        // Image sequences are video-only — no audio stream mapping.
        // For video containers, map the first audio stream.
        if (selectedFormat != ProxyFormat::ImageSequence) {
#ifdef __APPLE__
            arguments << QStringLiteral("-map") << QStringLiteral("0:a:0?");
#else
            arguments << QStringLiteral("-map") << QStringLiteral("0:a?");
#endif
        }
#ifdef __APPLE__
        arguments << QStringLiteral("-ignore_unknown");
#endif
    }

    const auto proxyFormat = static_cast<ProxyFormat>(
        formatCombo->currentData().toInt());
    const QString finalOutputPath = defaultProxyOutputPath(*clip, &sourceProbe, proxyFormat);
    const int proxyFps =
        qMax(1, qRound(sourceProbe.fps > 0.001 ? sourceProbe.fps : static_cast<double>(kTimelineFps)));
    const bool normalizeToCfr = !imageSequenceProxy;
    QStringList baseVideoFilters;
    if (normalizeToCfr) {
        baseVideoFilters << QStringLiteral("fps=%1").arg(proxyFps);
    }
    baseVideoFilters << QStringLiteral("scale='min(1280,iw)':-2");

    const bool alphaProxy = sourceProbe.hasAlpha;
    if (proxyFormat == ProxyFormat::ImageSequence)
    {
        // Image sequence proxy: extract frames as numbered JPEG files into a directory.
        // The directory serves as an image-sequence clip in the editor.
        const QDir outDir(finalOutputPath);
        if (!outDir.exists()) {
            outDir.mkpath(QStringLiteral("."));
        }
        arguments << QStringLiteral("-vf") << baseVideoFilters.join(QStringLiteral(","));
        if (continueGeneration && resumeNextFrameNumber > 1) {
            arguments << QStringLiteral("-start_number") << QString::number(resumeNextFrameNumber);
        }
        if (alphaProxy) {
            arguments << QStringLiteral("-pix_fmt") << QStringLiteral("rgba")
                      << QStringLiteral("%1/frame_%06d.png").arg(QDir(finalOutputPath).absolutePath());
        } else {
            arguments << QStringLiteral("-q:v") << QStringLiteral("3")
                      << QStringLiteral("%1/frame_%06d.jpg").arg(QDir(finalOutputPath).absolutePath());
        }
    }
    else
    {
        if (alphaProxy) {
            arguments << QStringLiteral("-vf") << baseVideoFilters.join(QStringLiteral(","));
            arguments << QStringLiteral("-c:v") << QStringLiteral("png")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("rgba");
        } else if (proxyFormat == ProxyFormat::H264) {
            arguments << QStringLiteral("-vf") << baseVideoFilters.join(QStringLiteral(","))
                      << QStringLiteral("-c:v") << QStringLiteral("libx264")
                      << QStringLiteral("-crf") << QStringLiteral("18")
                      << QStringLiteral("-preset") << QStringLiteral("ultrafast")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
        } else {
            // MJPEG
            arguments << QStringLiteral("-vf") << baseVideoFilters.join(QStringLiteral(","))
                      << QStringLiteral("-c:v") << QStringLiteral("mjpeg")
                      << QStringLiteral("-q:v") << QStringLiteral("3")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("yuvj420p");
        }
        if (!imageSequenceProxy) {
            arguments << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le")
                      << QStringLiteral("-b:a") << QStringLiteral("1536k");
        }
        arguments << QFileInfo(finalOutputPath).absoluteFilePath();
    }

    {
        const QString commandPreview = QStringLiteral("%1 %2")
                                           .arg(QDir::toNativeSeparators(ffmpegPath),
                                                arguments.join(QLatin1Char(' ')));
        QMessageBox confirm(this);
        confirm.setIcon(QMessageBox::Information);
        confirm.setWindowTitle(QStringLiteral("Confirm Proxy Command"));
        confirm.setText(QStringLiteral("Run ffmpeg to create this proxy?"));
        confirm.setInformativeText(
            QStringLiteral("Source VFR: %1\nCFR normalization: %2 (%3 fps)\nMode: %4")
                .arg(sourceIsVfr ? QStringLiteral("Yes") : QStringLiteral("No"))
                .arg(normalizeToCfr ? QStringLiteral("Enabled") : QStringLiteral("Disabled"))
                .arg(proxyFps)
                .arg(continueGeneration ? QStringLiteral("Continue from frame %1").arg(resumeNextFrameNumber)
                                        : QStringLiteral("Full regeneration")));
        confirm.setDetailedText(commandPreview);
        confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        confirm.setDefaultButton(QMessageBox::Yes);
        if (confirm.exec() != QMessageBox::Yes) {
            if (sequenceListFile) {
                QFile::remove(sequenceListFile->fileName());
            }
            return;
        }
    }

    const auto appendOutput = [output, autoScrollBox](const QString &text)
    {
        if (text.isEmpty()) return;
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
    };
        connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]()
            { appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput())); });
    connect(process, &QProcess::started, dialog, [appendOutput, ffmpegPath, arguments]()
            {
                appendOutput(QStringLiteral("$ %1 %2\n")
                                 .arg(QDir::toNativeSeparators(ffmpegPath),
                                      arguments.join(QLatin1Char(' '))));
            });
    connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error)
            { appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error))); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
            [this,
             clipId,
             outputPath = finalOutputPath,
             existingProxyPath,
             appendOutput,
             sourceFps = sourceProbe.fps,
             sourceDurationFrames = sourceProbe.durationFrames,
             sequenceListPath = sequenceListFile ? sequenceListFile->fileName() : QString()](int exitCode, QProcess::ExitStatus exitStatus)
            {
                appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                 .arg(exitCode)
                                 .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed")));
                if (!sequenceListPath.isEmpty())
                {
                    QFile::remove(sequenceListPath);
                }
                if (exitStatus != QProcess::NormalExit || exitCode != 0 || !QFileInfo::exists(outputPath))
                {
                    return;
                }
                if (!existingProxyPath.isEmpty() &&
                    existingProxyPath != outputPath &&
                    QFileInfo::exists(existingProxyPath))
                {
                    QFile::remove(existingProxyPath);
                }
                if (!m_timeline->updateClipById(
                        clipId,
                        [outputPath, sourceFps, sourceDurationFrames](TimelineClip &updatedClip)
                        {
                            updatedClip.proxyPath = outputPath;
                            if (sourceFps > 0.001) {
                                const bool hadLegacyTimelineFps =
                                    qAbs(updatedClip.sourceFps - static_cast<qreal>(kTimelineFps)) <= 0.001;
                                updatedClip.sourceFps = sourceFps;
                                if (hadLegacyTimelineFps &&
                                    updatedClip.sourceDurationFrames > 0 &&
                                    qAbs(updatedClip.durationFrames - updatedClip.sourceDurationFrames) <= 1) {
                                    updatedClip.durationFrames = qMax<int64_t>(
                                        1,
                                        qRound64((static_cast<qreal>(updatedClip.sourceDurationFrames) /
                                                  updatedClip.sourceFps) *
                                                 static_cast<qreal>(kTimelineFps)));
                                }
                            }
                            if (sourceDurationFrames > 0) {
                                updatedClip.sourceDurationFrames = sourceDurationFrames;
                            }
                        }))
                {
                    return;
                }
                if (m_timeline->clipsChanged)
                {
                    m_timeline->clipsChanged();
                }
            });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [process, sequenceListPath = sequenceListFile ? sequenceListFile->fileName() : QString()](int)
            {
                if (process->state() != QProcess::NotRunning)
                {
                    process->kill();
                    process->waitForFinished(1000);
                }
                if (!sequenceListPath.isEmpty())
                {
                    QFile::remove(sequenceListPath);
                }
            });

    process->start(ffmpegPath, arguments);
    dialog->show();
}

void EditorWindow::requestAutoSyncForSelection(const QSet<QString>& selectedClipIds)
{
    if (!m_timeline || selectedClipIds.isEmpty()) {
        return;
    }

    const QString backend = qEnvironmentVariable("SYNC_DETECTOR_BACKEND", QStringLiteral("auto")).trimmed();
    const QString extraArgs = qEnvironmentVariable("SYNC_DETECTOR_EXTRA_ARGS").trimmed();
    const QString runtime = qEnvironmentVariable("SYNC_DETECTOR_RUNTIME", QString()).trimmed().toLower();
    bool useDockerRuntime =
        (runtime == QStringLiteral("docker")) ||
        (backend.compare(QStringLiteral("syncnet"), Qt::CaseInsensitive) == 0);
    const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("sync_detector.py"));
    const bool scriptExists = QFileInfo::exists(scriptPath);
    QString pythonPath = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (!scriptExists) {
        QMessageBox::warning(this,
                             QStringLiteral("Sync Failed"),
                             QStringLiteral("sync_detector.py was not found at:\n%1")
                                 .arg(QDir::toNativeSeparators(scriptPath)));
        return;
    }
    {
        QMessageBox runtimeChoice(this);
        runtimeChoice.setIcon(QMessageBox::Information);
        runtimeChoice.setWindowTitle(QStringLiteral("Sync Runtime"));
        runtimeChoice.setText(QStringLiteral("Choose runtime for this sync run."));
        runtimeChoice.setInformativeText(
            QStringLiteral("Docker can use containerized dependencies. Local Python can be faster to start."));
        QPushButton* dockerButton =
            runtimeChoice.addButton(QStringLiteral("Use Docker"), QMessageBox::AcceptRole);
        QPushButton* localButton = nullptr;
        if (!pythonPath.isEmpty()) {
            localButton =
                runtimeChoice.addButton(QStringLiteral("Use Local Python"), QMessageBox::ActionRole);
        }
        runtimeChoice.addButton(QMessageBox::Cancel);
        runtimeChoice.setDefaultButton(useDockerRuntime ? dockerButton : localButton ? localButton : dockerButton);
        runtimeChoice.exec();
        if (runtimeChoice.clickedButton() == dockerButton) {
            useDockerRuntime = true;
        } else if (runtimeChoice.clickedButton() == localButton) {
            useDockerRuntime = false;
        } else {
            return;
        }
    }
    if (!useDockerRuntime && pythonPath.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Sync Failed"),
                             QStringLiteral("python3 was not found in PATH."));
        return;
    }

    if (m_renderInProgress) {
        QMessageBox::information(this, QStringLiteral("Sync"), QStringLiteral("A render is currently in progress."));
        return;
    }

    QVector<TimelineClip> clips = m_timeline->clips();
    QVector<const TimelineClip*> selected;
    selected.reserve(selectedClipIds.size());
    for (const TimelineClip& clip : clips) {
        if (selectedClipIds.contains(clip.id)) {
            selected.push_back(&clip);
        }
    }
    if (selected.isEmpty()) {
        return;
    }

    QVector<TimelineClip> visualClips;
    QVector<TimelineClip> audioClips;
    for (const TimelineClip* clip : selected) {
        if (!clip) {
            continue;
        }
        if (clipHasVisuals(*clip)) {
            visualClips.push_back(*clip);
        }
        if (clip->hasAudio || clip->mediaType == ClipMediaType::Audio) {
            audioClips.push_back(*clip);
        }
    }

    if (visualClips.isEmpty() || audioClips.isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("Sync"),
                                 QStringLiteral("Select clips containing both a visual source and an audio source."));
        return;
    }

    QString audioAnchorId;
    for (const TimelineClip& clip : audioClips) {
        if (!clipHasVisuals(clip)) {
            audioAnchorId = clip.id;
            break;
        }
    }
    if (audioAnchorId.isEmpty() && !audioClips.isEmpty()) {
        audioAnchorId = audioClips.constFirst().id;
    }
    if (audioAnchorId.isEmpty()) {
        return;
    }

    TimelineClip audioAnchorClip;
    bool foundAudioAnchor = false;
    for (const TimelineClip& clip : clips) {
        if (clip.id == audioAnchorId) {
            audioAnchorClip = clip;
            foundAudioAnchor = true;
            break;
        }
    }
    if (!foundAudioAnchor) {
        return;
    }

    const QString audioPath = playbackMediaPathForClip(audioAnchorClip);
    if (audioPath.isEmpty() || !QFileInfo::exists(audioPath)) {
        QMessageBox::warning(this,
                             QStringLiteral("Sync Failed"),
                             QStringLiteral("Audio source is unavailable for sync detection."));
        return;
    }

    QString dockerImage = qEnvironmentVariable("SYNCNET_DOCKER_IMAGE", QStringLiteral("syncnet-detector:latest")).trimmed();
    if (dockerImage.isEmpty()) {
        dockerImage = QStringLiteral("syncnet-detector:latest");
    }
    const QString effectiveBackend = backend.isEmpty() ? QStringLiteral("auto") : backend;
    const QString modelPath = qEnvironmentVariable("SYNCNET_MODEL_PATH", QString()).trimmed();
    const QString modelDevice = qEnvironmentVariable("SYNCNET_DEVICE", QStringLiteral("auto")).trimmed();
    bool intervalOk = false;
    const double intervalSeconds =
        qEnvironmentVariable("SYNC_DETECTOR_INTERVAL_SECONDS", QStringLiteral("5.0"))
            .trimmed()
            .toDouble(&intervalOk);
    const double effectiveIntervalSeconds =
        intervalOk ? qMax(0.1, intervalSeconds) : 5.0;
    bool windowOk = false;
    const double windowSeconds =
        qEnvironmentVariable("SYNC_DETECTOR_WINDOW_SECONDS", QStringLiteral("10.0"))
            .trimmed()
            .toDouble(&windowOk);
    const double effectiveWindowSeconds =
        windowOk ? qMax(0.1, windowSeconds) : 10.0;
    const QString intervalArg = QString::number(effectiveIntervalSeconds, 'g', 8);
    const QString windowArg = QString::number(effectiveWindowSeconds, 'g', 8);

    QStringList commandPreview;
    if (useDockerRuntime) {
        commandPreview.push_back(QStringLiteral("docker image inspect %1").arg(dockerImage));
        commandPreview.push_back(QStringLiteral("docker build -f %1 -t %2 %3")
                                     .arg(QStringLiteral("syncnet.dockerfile"),
                                          dockerImage,
                                          QDir::currentPath()));
        commandPreview.push_back(QStringLiteral(
                                     "docker run --rm [mounts incl: local sync_detector.py + source/dest workspace] "
                                     "--entrypoint python %1 /workspace/sync_detector.py "
                                     "--video ... --audio ... --fps %2 --interval-seconds %3 "
                                     "--window-seconds %4 --backend %5 --progress")
                                     .arg(dockerImage)
                                     .arg(kTimelineFps)
                                     .arg(intervalArg)
                                     .arg(windowArg)
                                     .arg(effectiveBackend));
    } else {
        commandPreview.push_back(QStringLiteral("%1 %2 --video ... --audio ... --fps %3 --interval-seconds %4 --window-seconds %5 --backend %6 --progress")
                                     .arg(QFileInfo(pythonPath).fileName(),
                                          QFileInfo(scriptPath).fileName())
                                     .arg(kTimelineFps)
                                     .arg(intervalArg)
                                     .arg(windowArg)
                                     .arg(effectiveBackend));
    }
    if (!extraArgs.isEmpty()) {
        commandPreview.push_back(QStringLiteral("extra args: %1").arg(extraArgs));
    }

    QMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Warning);
    confirm.setWindowTitle(QStringLiteral("Sync: Run External Commands"));
    confirm.setText(QStringLiteral("Sync will run external shell commands to analyze selected clips. Continue?"));
    confirm.setInformativeText(
        useDockerRuntime
            ? QStringLiteral("Runtime: Docker. This may build and run Docker containers and can take a while. No timeline changes are applied until you review and accept recommendations.")
            : QStringLiteral("Runtime: Local Python. This will run sync_detector.py in a shell process. No timeline changes are applied until you review and accept recommendations."));
    confirm.setDetailedText(commandPreview.join(QLatin1Char('\n')));
    confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Cancel);
    if (confirm.exec() != QMessageBox::Yes) {
        return;
    }

    QSet<QString> visualClipIds;
    visualClipIds.reserve(visualClips.size());
    for (const TimelineClip& clip : visualClips) {
        visualClipIds.insert(clip.id);
    }

    auto* terminalDialog = new QDialog(this);
    terminalDialog->setAttribute(Qt::WA_DeleteOnClose);
    terminalDialog->setWindowTitle(QStringLiteral("Sync Terminal"));
    terminalDialog->resize(980, 560);
    auto* terminalLayout = new QVBoxLayout(terminalDialog);
    terminalLayout->setContentsMargins(12, 12, 12, 12);
    terminalLayout->setSpacing(8);
    auto* statusLabel = new QLabel(QStringLiteral("Running sync..."), terminalDialog);
    terminalLayout->addWidget(statusLabel);
    auto* progressBar = new QProgressBar(terminalDialog);
    progressBar->setRange(0, visualClips.size());
    progressBar->setValue(0);
    terminalLayout->addWidget(progressBar);
    auto* output = new QPlainTextEdit(terminalDialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    terminalLayout->addWidget(output, 1);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), terminalDialog);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), terminalDialog);
    closeButton->setEnabled(false);
    buttonRow->addWidget(cancelButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    terminalLayout->addLayout(buttonRow);

    auto appendOutput = [output](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        output->moveCursor(QTextCursor::End);
    };
    terminalDialog->show();

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    connect(cancelButton, &QPushButton::clicked, terminalDialog, [cancelFlag]() {
        cancelFlag->store(true);
    });
    connect(closeButton, &QPushButton::clicked, terminalDialog, &QDialog::close);

    const QString previousSelected = m_timeline->selectedClipId();
    const QVector<TimelineClip> initialClips = m_timeline->clips();
    const QVector<RenderSyncMarker> initialMarkers = m_timeline->renderSyncMarkers();

    std::thread([this,
                 cancelFlag,
                 appendOutput,
                 statusLabel,
                 progressBar,
                 closeButton,
                 cancelButton,
                 terminalDialog,
                 initialClips,
                 initialMarkers,
                 visualClips,
                 visualClipIds,
                 audioAnchorId,
                 audioPath,
                 useDockerRuntime,
                 scriptPath,
                 pythonPath,
                 dockerImage,
                 effectiveBackend,
                 modelPath,
                 modelDevice,
                 intervalArg,
                 windowArg,
                 extraArgs,
                 previousSelected]() {
        struct ClipRecommendation {
            QString clipId;
            QString label;
            int videoOffsetFrames = 0;
            QVector<RenderSyncMarker> markers;
        };
        auto mergeMarkersWithMinSpacing = [](const QVector<RenderSyncMarker>& inputMarkers,
                                             const QString& clipId,
                                             int minSpacingFrames) {
            if (inputMarkers.isEmpty() || minSpacingFrames <= 0) {
                return inputMarkers;
            }

            QVector<RenderSyncMarker> markers = inputMarkers;
            std::sort(markers.begin(), markers.end(),
                      [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                          if (a.frame != b.frame) {
                              return a.frame < b.frame;
                          }
                          return a.count < b.count;
                      });

            auto markerDelta = [](const RenderSyncMarker& marker) {
                const int sign = marker.action == RenderSyncAction::DuplicateFrame ? 1 : -1;
                return sign * qMax(1, marker.count);
            };

            QVector<RenderSyncMarker> output;
            bool haveBucket = false;
            int64_t bucketFrame = 0;
            int bucketDelta = 0;
            auto flushBucket = [&]() {
                if (!haveBucket || bucketDelta == 0) {
                    haveBucket = false;
                    bucketDelta = 0;
                    return;
                }
                RenderSyncMarker merged;
                merged.clipId = clipId;
                merged.frame = bucketFrame;
                merged.action = bucketDelta > 0 ? RenderSyncAction::DuplicateFrame
                                                : RenderSyncAction::SkipFrame;
                merged.count = qMax(1, qAbs(bucketDelta));
                output.push_back(merged);
                haveBucket = false;
                bucketDelta = 0;
            };

            for (const RenderSyncMarker& marker : markers) {
                const int delta = markerDelta(marker);
                if (!haveBucket) {
                    haveBucket = true;
                    bucketFrame = marker.frame;
                    bucketDelta = delta;
                    continue;
                }
                if ((marker.frame - bucketFrame) < minSpacingFrames) {
                    bucketDelta += delta;
                    continue;
                }
                flushBucket();
                haveBucket = true;
                bucketFrame = marker.frame;
                bucketDelta = delta;
            }
            flushBucket();
            return output;
        };

        QVector<TimelineClip> clips = initialClips;
        QVector<RenderSyncMarker> nextMarkers = initialMarkers;
        nextMarkers.erase(std::remove_if(nextMarkers.begin(), nextMarkers.end(),
                                         [&](const RenderSyncMarker& marker) {
                                             return visualClipIds.contains(marker.clipId);
                                         }),
                          nextMarkers.end());
        QVector<ClipRecommendation> recommendations;
        int syncedCount = 0;
        QStringList skippedLocked;
        QStringList failures;

        auto uiLog = [terminalDialog, appendOutput](const QString& line) {
            QMetaObject::invokeMethod(terminalDialog, [appendOutput, line]() { appendOutput(line); }, Qt::QueuedConnection);
        };
        auto uiProgress = [terminalDialog, progressBar](int value) {
            QMetaObject::invokeMethod(terminalDialog, [progressBar, value]() { progressBar->setValue(value); }, Qt::QueuedConnection);
        };
        auto uiStatus = [terminalDialog, statusLabel](const QString& text) {
            QMetaObject::invokeMethod(terminalDialog, [statusLabel, text]() { statusLabel->setText(text); }, Qt::QueuedConnection);
        };

        if (useDockerRuntime) {
            const QString dockerPath = QStandardPaths::findExecutable(QStringLiteral("docker"));
            if (dockerPath.isEmpty()) {
                failures.push_back(QStringLiteral("Docker runtime requested, but docker was not found in PATH."));
                cancelFlag->store(true);
            } else if (!QFileInfo::exists(scriptPath)) {
                failures.push_back(QStringLiteral("sync_detector.py not found at %1").arg(scriptPath));
                cancelFlag->store(true);
            } else {
                uiStatus(QStringLiteral("Preparing Docker runtime..."));
                const QString inspectCommand = QStringLiteral("docker image inspect %1 >/dev/null 2>&1")
                                                   .arg(shellQuote(dockerImage));
                const ShellRunResult inspectResult = runShellCommandStreaming(inspectCommand, cancelFlag.get(), uiLog, 60000);
                if (!inspectResult.canceled &&
                    (inspectResult.exitStatus != QProcess::NormalExit || inspectResult.exitCode != 0)) {
                    const QString dockerfilePath =
                        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("syncnet.dockerfile"));
                    if (!QFileInfo::exists(dockerfilePath)) {
                        failures.push_back(QStringLiteral("Docker image missing and syncnet.dockerfile not found."));
                        cancelFlag->store(true);
                    } else {
                        const QString buildCommand = QStringLiteral("docker build -f %1 -t %2 %3")
                                                         .arg(shellQuote(dockerfilePath))
                                                         .arg(shellQuote(dockerImage))
                                                         .arg(shellQuote(QDir::currentPath()));
                        uiStatus(QStringLiteral("Building Docker image..."));
                        const ShellRunResult buildResult = runShellCommandStreaming(buildCommand, cancelFlag.get(), uiLog, -1);
                        if (buildResult.canceled) {
                            cancelFlag->store(true);
                        } else if (buildResult.exitStatus != QProcess::NormalExit || buildResult.exitCode != 0) {
                            failures.push_back(QStringLiteral("Failed to build Docker image %1.").arg(dockerImage));
                            cancelFlag->store(true);
                        }
                    }
                } else if (inspectResult.canceled) {
                    cancelFlag->store(true);
                }
            }
        }

        for (int i = 0; i < visualClips.size(); ++i) {
            uiProgress(i);
            if (cancelFlag->load()) {
                break;
            }

            const TimelineClip visualClip = visualClips[i];
            if (visualClip.locked) {
                skippedLocked.push_back(visualClip.label);
                continue;
            }

            const QString videoPath = playbackMediaPathForClip(visualClip);
            if (videoPath.isEmpty() || !QFileInfo::exists(videoPath)) {
                failures.push_back(QStringLiteral("%1: missing visual source").arg(visualClip.label));
                continue;
            }

            QString command;
            if (useDockerRuntime) {
                QString modelArg;
                QVector<QPair<QString, QString>> mounts;
                auto mountPath = [&mounts](const QString& hostFile, const QString& prefix) -> QString {
                    const QFileInfo info(hostFile);
                    const QString hostDir = info.absolutePath();
                    for (const auto& existing : std::as_const(mounts)) {
                        if (existing.first == hostDir) {
                            return existing.second + QStringLiteral("/") + info.fileName();
                        }
                    }
                    const QString target = QStringLiteral("/%1%2").arg(prefix).arg(mounts.size());
                    mounts.push_back(qMakePair(hostDir, target));
                    return target + QStringLiteral("/") + info.fileName();
                };
                const QString containerVideoPath = mountPath(videoPath, QStringLiteral("input"));
                const QString containerAudioPath = mountPath(audioPath, QStringLiteral("input"));
                if (!modelPath.isEmpty()) {
                    const QString containerModelPath = mountPath(modelPath, QStringLiteral("model"));
                    modelArg = QStringLiteral(" --syncnet-model %1 --syncnet-device %2")
                                   .arg(shellQuote(containerModelPath),
                                        shellQuote(modelDevice.isEmpty() ? QStringLiteral("auto") : modelDevice));
                }
                QString gpuArg;
                if (qEnvironmentVariable("SYNCNET_DOCKER_GPU", QStringLiteral("1")).trimmed() != QStringLiteral("0")) {
                    gpuArg = QStringLiteral(" --gpus all");
                }
                QString mountArgs;
                for (const auto& mount : std::as_const(mounts)) {
                    mountArgs += QStringLiteral(" -v %1:%2:ro").arg(shellQuote(mount.first), shellQuote(mount.second));
                }
                const QString workspaceHostDir = QDir::currentPath();
                const QString scriptContainerPath = QStringLiteral("/workspace/sync_detector.py");
                const QString workspaceContainerDir = QStringLiteral("/workspace-host");
                mountArgs += QStringLiteral(" -v %1:%2:ro")
                                 .arg(shellQuote(scriptPath),
                                      shellQuote(scriptContainerPath));
                mountArgs += QStringLiteral(" -v %1:%2:rw")
                                 .arg(shellQuote(workspaceHostDir),
                                      shellQuote(workspaceContainerDir));
                command = QStringLiteral(
                              "docker run --rm%1%2 -w %3 --entrypoint python %4 %5 "
                              "--video %6 --audio %7 --fps %8 --interval-seconds %9 --window-seconds %10 --backend %11 --progress%12")
                              .arg(gpuArg)
                              .arg(mountArgs)
                              .arg(shellQuote(workspaceContainerDir))
                              .arg(shellQuote(dockerImage))
                              .arg(shellQuote(scriptContainerPath))
                              .arg(shellQuote(containerVideoPath))
                              .arg(shellQuote(containerAudioPath))
                              .arg(kTimelineFps)
                              .arg(shellQuote(intervalArg))
                              .arg(shellQuote(windowArg))
                              .arg(shellQuote(effectiveBackend))
                              .arg(modelArg);
            } else {
                command = QStringLiteral("%1 %2 --video %3 --audio %4 --fps %5 --interval-seconds %6 --window-seconds %7 --backend %8")
                              .arg(shellQuote(pythonPath))
                              .arg(shellQuote(scriptPath))
                              .arg(shellQuote(videoPath))
                              .arg(shellQuote(audioPath))
                              .arg(kTimelineFps)
                              .arg(shellQuote(intervalArg))
                              .arg(shellQuote(windowArg))
                              .arg(shellQuote(effectiveBackend));
                command += QStringLiteral(" --progress");
            }
            if (!extraArgs.isEmpty()) {
                command += QStringLiteral(" ");
                command += extraArgs;
            }

            const ShellRunResult detectorRun = runShellCommandStreaming(command, cancelFlag.get(), uiLog, -1);
            if (detectorRun.canceled || cancelFlag->load()) {
                cancelFlag->store(true);
                break;
            }
            const QString detectorOutput = detectorRun.output;
            if (detectorRun.exitStatus != QProcess::NormalExit || detectorRun.exitCode != 0) {
                failures.push_back(QStringLiteral("%1: detector failed (%2)")
                                       .arg(visualClip.label,
                                            detectorOutput.trimmed().isEmpty() ? detectorRun.errorString : detectorOutput.trimmed()));
                continue;
            }

            const QString jsonPayload = extractJsonObject(detectorOutput);
            QJsonParseError parseError;
            const QJsonDocument json = QJsonDocument::fromJson(jsonPayload.toUtf8(), &parseError);
            if (json.isNull() || !json.isObject()) {
                failures.push_back(QStringLiteral("%1: invalid detector output").arg(visualClip.label));
                continue;
            }

            const QJsonObject root = json.object();
            const int videoOffsetFrames = root.value(QStringLiteral("videoOffsetFrames")).toInt(0);
            int64_t appliedClipStartFrame = visualClip.startFrame;
            for (TimelineClip& mutableClip : clips) {
                if (mutableClip.id != visualClip.id) {
                    continue;
                }
                if (mutableClip.id != audioAnchorId && videoOffsetFrames != 0) {
                    mutableClip.startFrame = qMax<int64_t>(0, mutableClip.startFrame + videoOffsetFrames);
                    normalizeClipTiming(mutableClip);
                }
                appliedClipStartFrame = mutableClip.startFrame;
                break;
            }

            ClipRecommendation recommendation;
            recommendation.clipId = visualClip.id;
            recommendation.label = visualClip.label;
            recommendation.videoOffsetFrames = videoOffsetFrames;

            const QJsonArray markerArray = root.value(QStringLiteral("markers")).toArray();
            for (const QJsonValue& markerValue : markerArray) {
                if (!markerValue.isObject()) {
                    continue;
                }
                const QJsonObject markerObj = markerValue.toObject();
                RenderSyncMarker marker;
                marker.clipId = visualClip.id;
                marker.count = qMax(1, markerObj.value(QStringLiteral("count")).toInt(1));
                if (!parseSyncAction(markerObj.value(QStringLiteral("action")).toString(), &marker.action)) {
                    continue;
                }

                const QJsonValue timelineFrameValue = markerObj.value(QStringLiteral("timelineFrame"));
                if (timelineFrameValue.isDouble()) {
                    marker.frame = qMax<int64_t>(0, timelineFrameValue.toVariant().toLongLong());
                } else {
                    const int64_t localFrame = qMax<int64_t>(0, markerObj.value(QStringLiteral("frame")).toVariant().toLongLong());
                    marker.frame = qMax<int64_t>(0, appliedClipStartFrame + localFrame);
                }
                recommendation.markers.push_back(marker);
                nextMarkers.push_back(marker);
            }
            recommendations.push_back(recommendation);
            uiLog(QStringLiteral("Recommendation for %1: shift %2 frame(s), new sync points: %3\n")
                      .arg(visualClip.label,
                           QString::number(videoOffsetFrames),
                           QString::number(recommendation.markers.size())));
            ++syncedCount;
        }
        uiProgress(visualClips.size());

        QMetaObject::invokeMethod(terminalDialog, [=, this]() {
            cancelButton->setEnabled(false);
            closeButton->setEnabled(true);
            if (cancelFlag->load() && syncedCount == 0) {
                statusLabel->setText(QStringLiteral("Sync canceled"));
                return;
            }

            QStringList recommendationLines;
            int clipsWithAdjustments = 0;
            int totalSuggestedMarkers = 0;
            for (const ClipRecommendation& recommendation : recommendations) {
                const bool hasAdjustment =
                    (recommendation.videoOffsetFrames != 0) || !recommendation.markers.isEmpty();
                if (!hasAdjustment) {
                    continue;
                }
                ++clipsWithAdjustments;
                totalSuggestedMarkers += recommendation.markers.size();
                QString line = QStringLiteral("%1: shift %2 frame(s), sync points %3")
                                   .arg(recommendation.label,
                                        QString::number(recommendation.videoOffsetFrames),
                                        QString::number(recommendation.markers.size()));
                if (!recommendation.markers.isEmpty()) {
                    QStringList markerFrames;
                    const int markerPreviewCount = qMin(8, recommendation.markers.size());
                    for (int markerIndex = 0; markerIndex < markerPreviewCount; ++markerIndex) {
                        const RenderSyncMarker& marker = recommendation.markers[markerIndex];
                        const QString action =
                            marker.action == RenderSyncAction::DuplicateFrame
                                ? QStringLiteral("dup")
                                : QStringLiteral("skip");
                        markerFrames.push_back(
                            QStringLiteral("%1:%2x%3")
                                .arg(QString::number(marker.frame),
                                     action,
                                     QString::number(marker.count)));
                    }
                    line += QStringLiteral(" [") + markerFrames.join(QStringLiteral(", ")) + QStringLiteral("]");
                }
                recommendationLines.push_back(line);
            }

            QString summary = QStringLiteral("Analyzed %1 clip%2.")
                                  .arg(syncedCount)
                                  .arg(syncedCount == 1 ? QString() : QStringLiteral("s"));
            if (cancelFlag->load()) {
                summary += QStringLiteral(" (canceled)");
            }
            if (!recommendationLines.isEmpty()) {
                summary += QStringLiteral("\n\nRecommended adjustments:\n- %1")
                               .arg(recommendationLines.join(QStringLiteral("\n- ")));
            } else {
                summary += QStringLiteral("\n\nNo non-zero sync adjustments were recommended.");
            }
            if (!failures.isEmpty()) {
                summary += QStringLiteral("\n\nFailures:\n- %1").arg(failures.join(QStringLiteral("\n- ")));
            }
            if (!skippedLocked.isEmpty()) {
                summary += QStringLiteral("\n\nLocked clips skipped:\n- %1").arg(skippedLocked.join(QStringLiteral("\n- ")));
            }
            appendOutput(QStringLiteral("\n%1\n").arg(summary));

            bool applied = false;
            if (syncedCount > 0 && clipsWithAdjustments > 0) {
                bool spacingAccepted = false;
                bool hasDefaultSpacing = false;
                int defaultSpacing = qEnvironmentVariableIntValue("SYNC_MIN_MARKER_SPACING_FRAMES",
                                                                  &hasDefaultSpacing);
                if (!hasDefaultSpacing) {
                    defaultSpacing = 6;
                }
                const int minSpacingFrames = QInputDialog::getInt(
                    terminalDialog,
                    QStringLiteral("Sync Options"),
                    QStringLiteral("Minimum spacing between sync points (frames).\n"
                                   "Points closer than this are merged and counts recalculated.\n"
                                   "Use 0 to keep all points."),
                    qMax(0, defaultSpacing),
                    0,
                    10000,
                    1,
                    &spacingAccepted);
                if (!spacingAccepted) {
                    appendOutput(QStringLiteral("Recommendations were not applied (options canceled).\n"));
                    summary += QStringLiteral("\n\nNot applied.");
                    statusLabel->setText(summary);
                    return;
                }

                QMessageBox applyConfirm(terminalDialog);
                applyConfirm.setIcon(QMessageBox::Question);
                applyConfirm.setWindowTitle(QStringLiteral("Apply Sync Recommendations"));
                applyConfirm.setText(
                    QStringLiteral("Apply recommended shift(s) and sync point(s)?"));
                applyConfirm.setInformativeText(
                    QStringLiteral("Clips with adjustments: %1\nSuggested sync points: %2\nMin spacing: %3 frame(s)")
                        .arg(clipsWithAdjustments)
                        .arg(totalSuggestedMarkers)
                        .arg(minSpacingFrames));
                applyConfirm.setDetailedText(recommendationLines.join(QLatin1Char('\n')));
                applyConfirm.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                applyConfirm.setDefaultButton(QMessageBox::Yes);
                if (applyConfirm.exec() == QMessageBox::Yes) {
                    QVector<RenderSyncMarker> filteredMarkers;
                    filteredMarkers.reserve(nextMarkers.size());
                    const int unaffectedCount = nextMarkers.size() - totalSuggestedMarkers;
                    if (unaffectedCount > 0) {
                        for (const RenderSyncMarker& marker : nextMarkers) {
                            if (!visualClipIds.contains(marker.clipId)) {
                                filteredMarkers.push_back(marker);
                            }
                        }
                    }
                    int recalculatedMarkers = 0;
                    for (const ClipRecommendation& recommendation : recommendations) {
                        const QVector<RenderSyncMarker> mergedMarkers =
                            mergeMarkersWithMinSpacing(recommendation.markers,
                                                       recommendation.clipId,
                                                       minSpacingFrames);
                        recalculatedMarkers += mergedMarkers.size();
                        for (const RenderSyncMarker& marker : mergedMarkers) {
                            filteredMarkers.push_back(marker);
                        }
                    }

                    const bool previousSuppress = m_suppressHistorySnapshots;
                    m_suppressHistorySnapshots = true;
                    m_timeline->setClips(clips);
                    m_timeline->setRenderSyncMarkers(filteredMarkers);
                    if (!previousSelected.isEmpty()) {
                        m_timeline->setSelectedClipId(previousSelected);
                    }
                    m_suppressHistorySnapshots = previousSuppress;
                    scheduleSaveState();
                    pushHistorySnapshot();
                    applied = true;
                    appendOutput(QStringLiteral("Sync points recalculated: %1 -> %2 (min spacing %3 frame(s)).\n")
                                     .arg(totalSuggestedMarkers)
                                     .arg(recalculatedMarkers)
                                     .arg(minSpacingFrames));
                    appendOutput(QStringLiteral("Applied recommended sync updates.\n"));
                } else {
                    appendOutput(QStringLiteral("Recommendations were not applied.\n"));
                }
            }

            if (applied) {
                summary += QStringLiteral("\n\nApplied.");
            } else if (clipsWithAdjustments > 0) {
                summary += QStringLiteral("\n\nNot applied.");
            }
            statusLabel->setText(summary);
        }, Qt::QueuedConnection);
    }).detach();
}

void EditorWindow::deleteProxyForClip(const QString &clipId)
{
    if (!m_timeline) return;

    const TimelineClip *clip = nullptr;
    for (const TimelineClip &candidate : m_timeline->clips())
    {
        if (candidate.id == clipId)
        {
            clip = &candidate;
            break;
        }
    }
    if (!clip) return;

    const QString proxyPath = playbackProxyPathForClip(*clip);
    if (proxyPath.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Delete Proxy"),
                                 QStringLiteral("No proxy exists for this clip."));
        return;
    }

    const auto response = QMessageBox::question(
        this,
        QStringLiteral("Delete Proxy"),
        QStringLiteral("Delete this proxy?\n%1").arg(QDir::toNativeSeparators(proxyPath)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) return;

    const QFileInfo proxyInfo(proxyPath);
    if (proxyInfo.isDir()) {
        QDir dir(proxyPath);
        dir.removeRecursively();
    } else {
        QFile::remove(proxyPath);
    }
    if (!m_timeline->updateClipById(clipId, [](TimelineClip &updatedClip)
                                   { updatedClip.proxyPath.clear(); }))
    {
        return;
    }
    if (m_timeline->clipsChanged)
    {
        m_timeline->clipsChanged();
    }
}
