#include "editor.h"
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextCursor>
#include <QTextStream>
#include <QVBoxLayout>

using namespace editor;

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
void EditorWindow::createProxyForClip(const QString &clipId)
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
    const MediaProbeResult sourceProbe = probeMediaFile(probePath, clip->durationFrames);
    const QString existingProxyPath = playbackProxyPathForClip(*clip);
    const QString outputPath = defaultProxyOutputPath(*clip, &sourceProbe, ProxyFormat::ImageSequence);
    const QString overwriteTarget = !existingProxyPath.isEmpty() ? existingProxyPath : outputPath;
    
    if (QFileInfo::exists(overwriteTarget))
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
    dialog->setWindowTitle(QStringLiteral("Create Proxy  %1").arg(clip->label));
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
        for (const QString &framePath : sequenceFrames)
        {
            QString escapedFramePath = QFileInfo(framePath).absoluteFilePath();
            escapedFramePath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            stream << "file '" << escapedFramePath << "'\n";
            stream << "duration " << QString::number(1.0 / static_cast<double>(kTimelineFps), 'f', 6) << '\n';
        }
        if (!sequenceFrames.isEmpty())
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

    const bool alphaProxy = sourceProbe.hasAlpha;
    if (proxyFormat == ProxyFormat::ImageSequence)
    {
        // Image sequence proxy: extract frames as numbered JPEG files into a directory.
        // The directory serves as an image-sequence clip in the editor.
        const QDir outDir(finalOutputPath);
        if (!outDir.exists()) {
            QDir().mkpath(finalOutputPath);
        }
        arguments << QStringLiteral("-vf") << QStringLiteral("scale='min(1280,iw)':-2");
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
            arguments << QStringLiteral("-c:v") << QStringLiteral("png")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("rgba");
        } else if (proxyFormat == ProxyFormat::H264) {
            arguments << QStringLiteral("-vf") << QStringLiteral("scale='min(1280,iw)':-2")
                      << QStringLiteral("-c:v") << QStringLiteral("libx264")
                      << QStringLiteral("-crf") << QStringLiteral("18")
                      << QStringLiteral("-preset") << QStringLiteral("ultrafast")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
        } else {
            // MJPEG
            arguments << QStringLiteral("-vf") << QStringLiteral("scale='min(1280,iw)':-2")
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
            [this, clipId, outputPath = finalOutputPath, existingProxyPath, appendOutput, sequenceListPath = sequenceListFile ? sequenceListFile->fileName() : QString()](int exitCode, QProcess::ExitStatus exitStatus)
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
                if (!m_timeline->updateClipById(clipId, [outputPath](TimelineClip &updatedClip)
                                               { updatedClip.proxyPath = outputPath; }))
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

    QFile::remove(proxyPath);
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

