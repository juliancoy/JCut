#include "editor.h"
#include "facedetections_artifact_utils.h"
#include "history_tab.h"
#include "mask_tab.h"
#include "processing_job_docker.h"
#include "playback_debug.h"
#include "processing_job_manifest.h"
#include "speakers_table.h"
#include "transform_skip_aware_timing.h"

#include <QColorDialog>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>

using namespace editor;

namespace {

bool inspectorTabRefreshIsHeavyDuringPlayback(const QString& tabName)
{
    const QString normalized = tabName.trimmed();
    return normalized.compare(QStringLiteral("Transcript"), Qt::CaseInsensitive) == 0 ||
           normalized.compare(QStringLiteral("Speakers"), Qt::CaseInsensitive) == 0;
}

QString jobInputLabel(const QJsonObject& manifest)
{
    const QString inputPath =
        manifest.value(QStringLiteral("input")).toObject().value(QStringLiteral("path")).toString();
    return QFileInfo(inputPath).fileName().isEmpty()
        ? inputPath
        : QFileInfo(inputPath).fileName();
}

QString jobProcessLabel(const QJsonObject& manifest)
{
    const QJsonObject process = manifest.value(QStringLiteral("process")).toObject();
    const QJsonObject docker = process.value(QStringLiteral("docker")).toObject();
    const QString containerName = docker.value(QStringLiteral("container_name")).toString();
    if (!containerName.isEmpty()) {
        return QStringLiteral("Docker %1").arg(containerName);
    }
    const QString legacyContainerName = manifest.value(QStringLiteral("docker_container_name")).toString();
    if (!legacyContainerName.isEmpty()) {
        return QStringLiteral("Docker %1").arg(legacyContainerName);
    }
    const QJsonValue pidValue = manifest.value(QStringLiteral("pid"));
    if (pidValue.isDouble()) {
        return QStringLiteral("PID %1").arg(pidValue.toVariant().toLongLong());
    }
    if (manifest.contains(QStringLiteral("exit_code"))) {
        return QStringLiteral("exit %1").arg(manifest.value(QStringLiteral("exit_code")).toInt());
    }
    const QString pauseReason = manifest.value(QStringLiteral("pause_reason")).toString();
    if (!pauseReason.isEmpty()) {
        return pauseReason;
    }
    return QString();
}

QString jobStatusLabel(const QJsonObject& manifest,
                       const jcut::jobs::DockerContainerInfo* container)
{
    if (container) {
        return jcut::jobs::dockerContainerIsRunning(*container)
            ? QStringLiteral("running")
            : container->status;
    }
    return manifest.value(QStringLiteral("status")).toString();
}

QString jobProcessLabel(const QJsonObject& manifest,
                        const jcut::jobs::DockerContainerInfo* container)
{
    if (container) {
        const QString name = jcut::jobs::dockerContainerIdentifier(*container);
        return container->status.isEmpty()
            ? QStringLiteral("Docker %1").arg(name)
            : QStringLiteral("Docker %1 (%2)").arg(name, container->status);
    }
    return jobProcessLabel(manifest);
}

QJsonObject manifestWithDockerProcess(const QJsonObject& manifest,
                                      const jcut::jobs::DockerContainerInfo& container)
{
    QJsonObject patched = manifest;
    QJsonObject process = patched.value(QStringLiteral("process")).toObject();
    QJsonObject docker = process.value(QStringLiteral("docker")).toObject();
    docker.insert(QStringLiteral("container_id"), container.id);
    docker.insert(QStringLiteral("container_name"), container.name);
    docker.insert(QStringLiteral("image"), container.image);
    docker.insert(QStringLiteral("status"), container.status);
    process.insert(QStringLiteral("type"), QStringLiteral("docker"));
    process.insert(QStringLiteral("docker"), docker);
    patched.insert(QStringLiteral("process"), process);
    patched.insert(QStringLiteral("docker_container_name"), container.name);
    return patched;
}

void addManifestPath(QStringList* paths, QSet<QString>* seen, const QString& path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (absolute.isEmpty() || seen->contains(absolute) || !QFileInfo::exists(absolute)) {
        return;
    }
    seen->insert(absolute);
    paths->append(absolute);
}

void addManifestDir(QStringList* paths, QSet<QString>* seen, const QString& dirPath)
{
    const QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }
    QDirIterator it(dir.absolutePath(),
                    QStringList{QStringLiteral("manifest.json")},
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        addManifestPath(paths, seen, it.next());
    }
}

} // namespace

void EditorWindow::refreshInspectorTabByName(const QString& tabName)
{
    const QString normalized = tabName.trimmed();
    if (playbackActive() && inspectorTabRefreshIsHeavyDuringPlayback(normalized)) {
        return;
    }
    if (normalized.compare(QStringLiteral("Grade"), Qt::CaseInsensitive) == 0) {
        if (m_gradingTab) m_gradingTab->refresh();
    } else if (normalized.compare(QStringLiteral("Opacity"), Qt::CaseInsensitive) == 0) {
        if (m_opacityTab) m_opacityTab->refresh();
    } else if (normalized.compare(QStringLiteral("Effects"), Qt::CaseInsensitive) == 0) {
        if (m_effectsTab) m_effectsTab->refresh();
    } else if (normalized.compare(QStringLiteral("Masks"), Qt::CaseInsensitive) == 0) {
        if (m_maskTab) m_maskTab->refresh();
    } else if (normalized.compare(QStringLiteral("Corrections"), Qt::CaseInsensitive) == 0) {
        if (m_correctionsTab) m_correctionsTab->refresh();
    } else if (normalized.compare(QStringLiteral("Titles"), Qt::CaseInsensitive) == 0) {
        if (m_titlesTab) m_titlesTab->refresh();
    } else if (normalized.compare(QStringLiteral("Sync"), Qt::CaseInsensitive) == 0) {
        if (m_syncTab) m_syncTab->refresh();
    } else if (normalized.compare(QStringLiteral("Transform"), Qt::CaseInsensitive) == 0 ||
               normalized.compare(QStringLiteral("Keyframes"), Qt::CaseInsensitive) == 0) {
        if (m_videoKeyframeTab) m_videoKeyframeTab->refresh();
    } else if (normalized.compare(QStringLiteral("Transcript"), Qt::CaseInsensitive) == 0) {
        if (m_transcriptTab) m_transcriptTab->refresh();
    } else if (normalized.compare(QStringLiteral("Speakers"), Qt::CaseInsensitive) == 0) {
        if (m_speakersTab) m_speakersTab->refresh();
        if (m_speakerTranscriptTab) m_speakerTranscriptTab->refresh();
    } else if (normalized.compare(QStringLiteral("Properties"), Qt::CaseInsensitive) == 0) {
        if (m_propertiesTab) m_propertiesTab->refresh();
    } else if (normalized.compare(QStringLiteral("Clips"), Qt::CaseInsensitive) == 0) {
        if (m_clipsTab) m_clipsTab->refresh();
    } else if (normalized.compare(QStringLiteral("History"), Qt::CaseInsensitive) == 0) {
        if (m_historyTab) m_historyTab->refresh();
    } else if (normalized.compare(QStringLiteral("Tracks"), Qt::CaseInsensitive) == 0) {
        if (m_tracksTab) m_tracksTab->refresh();
    } else if (normalized.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0) {
        refreshAudioInspectorViews();
    } else if (normalized.compare(QStringLiteral("Jobs"), Qt::CaseInsensitive) == 0) {
        refreshProcessingJobsTab();
    } else if (normalized.compare(QStringLiteral("Output"), Qt::CaseInsensitive) == 0) {
        if (m_outputTab) m_outputTab->refresh();
    } else if (normalized.compare(QStringLiteral("Pipeline"), Qt::CaseInsensitive) == 0) {
        if (m_pipelineTab) m_pipelineTab->refresh();
    } else if (normalized.compare(QStringLiteral("System"), Qt::CaseInsensitive) == 0) {
        if (m_profileTab) m_profileTab->refresh();
    } else if (normalized.compare(QStringLiteral("Projects"), Qt::CaseInsensitive) == 0) {
        if (m_projectsTab) m_projectsTab->refresh();
    }
}

void EditorWindow::refreshProcessingJobsTab()
{
    if (!m_inspectorPane || !m_inspectorPane->processingJobsTable()) {
        return;
    }

    QStringList manifestPaths;
    QSet<QString> seenPaths;
    const QVector<TimelineClip> clips = m_timeline ? m_timeline->clips() : QVector<TimelineClip>{};
    if (m_explorerPane) {
        const QStringList explorerJobRoots{
            m_explorerPane->currentRootPath(),
            m_explorerPane->galleryPath(),
        };
        for (const QString& root : explorerJobRoots) {
            if (!root.trimmed().isEmpty()) {
                addManifestDir(&manifestPaths,
                               &seenPaths,
                               QDir(root).absoluteFilePath(QStringLiteral(".jcut_jobs")));
            }
        }
    }
    for (const TimelineClip& clip : clips) {
        const QString mediaPath = clip.filePath.trimmed();
        if (mediaPath.isEmpty()) {
            continue;
        }
        const QFileInfo mediaInfo(mediaPath);
        addManifestDir(&manifestPaths,
                       &seenPaths,
                       mediaInfo.dir().absoluteFilePath(QStringLiteral(".jcut_jobs")));
        addManifestPath(&manifestPaths,
                        &seenPaths,
                        QDir(facedetectionsClipSidecarDir(mediaPath, clip.id))
                            .absoluteFilePath(QStringLiteral("manifest.json")));
    }

    struct JobRow {
        QJsonObject manifest;
        QString manifestPath;
        QDateTime updated;
        jcut::jobs::DockerContainerInfo dockerContainer;
        bool hasDockerContainer = false;
    };
    QString dockerError;
    const QVector<jcut::jobs::DockerContainerInfo> dockerContainers =
        jcut::jobs::listDockerContainers(&dockerError);
    QVector<JobRow> rows;
    rows.reserve(manifestPaths.size());
    for (const QString& path : manifestPaths) {
        QJsonObject manifest;
        if (!jcut::jobs::readManifest(path, &manifest, nullptr) ||
            manifest.value(QStringLiteral("schema")).toString() != QStringLiteral("jcut_processing_job_v1")) {
            continue;
        }
        JobRow row{
            manifest,
            path,
            QDateTime::fromString(
                manifest.value(QStringLiteral("updated_at_utc")).toString(),
                Qt::ISODate),
        };
        if (const jcut::jobs::DockerContainerInfo* container =
                jcut::jobs::findDockerContainerForManifest(manifest, dockerContainers)) {
            row.dockerContainer = *container;
            row.hasDockerContainer = true;
            row.manifest = manifestWithDockerProcess(row.manifest, *container);
            if (jcut::jobs::dockerContainerIsRunning(*container)) {
                const QString manifestContainerName =
                    jcut::jobs::dockerContainerNameFromManifest(manifest);
                const bool manifestNeedsDockerPatch =
                    manifest.value(QStringLiteral("status")).toString() != QStringLiteral("running") ||
                    manifestContainerName != container->name ||
                    jcut::jobs::dockerContainerIdFromManifest(manifest) != container->id;
                if (manifestNeedsDockerPatch) {
                    jcut::jobs::updateManifestStatus(path, QStringLiteral("running"), row.manifest, nullptr);
                }
            }
        }
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const JobRow& a, const JobRow& b) {
        return a.updated > b.updated;
    });

    int runningCount = 0;
    int queuedCount = 0;
    for (const JobRow& row : rows) {
        const QString status = jobStatusLabel(
            row.manifest,
            row.hasDockerContainer ? &row.dockerContainer : nullptr);
        if (status.compare(QStringLiteral("running"), Qt::CaseInsensitive) == 0) {
            ++runningCount;
        } else if (status.compare(QStringLiteral("prepared"), Qt::CaseInsensitive) == 0 ||
                   status.compare(QStringLiteral("queued"), Qt::CaseInsensitive) == 0) {
            ++queuedCount;
        }
    }

    if (QLabel* label = m_inspectorPane->processingJobsSummaryLabel()) {
        QString text = QStringLiteral("%1 job(s) found. Running: %2. Queued/prepared: %3.")
                           .arg(rows.size())
                           .arg(runningCount)
                           .arg(queuedCount);
        if (!dockerError.isEmpty()) {
            text += QStringLiteral(" Docker unavailable: %1.").arg(dockerError);
        }
        label->setText(text);
    }

    QTableWidget* table = m_inspectorPane->processingJobsTable();
    QSignalBlocker blocker(table);
    table->clearContents();
    table->setRowCount(rows.size());

    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const JobRow& row = rows.at(rowIndex);
        const QJsonObject& manifest = row.manifest;
        const QString operation = manifest.value(QStringLiteral("operation")).toString();
        const QString status = jobStatusLabel(
            manifest,
            row.hasDockerContainer ? &row.dockerContainer : nullptr);
        const QString updated = row.updated.isValid()
            ? row.updated.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : manifest.value(QStringLiteral("updated_at_utc")).toString();
        const QString process = jobProcessLabel(
            manifest,
            row.hasDockerContainer ? &row.dockerContainer : nullptr);

        const QStringList values{
            operation,
            status,
            jobInputLabel(manifest),
            updated,
            process,
            QDir::toNativeSeparators(row.manifestPath),
        };
        for (int col = 0; col < values.size(); ++col) {
            auto* item = new QTableWidgetItem(values.at(col));
            item->setToolTip(values.at(col));
            table->setItem(rowIndex, col, item);
        }
        auto* logButton = new QPushButton(QStringLiteral("Open Log"), table);
        logButton->setObjectName(QStringLiteral("jobs.open_log"));
        logButton->setEnabled(row.hasDockerContainer);
        logButton->setToolTip(row.hasDockerContainer
                                  ? QStringLiteral("Open this job's Docker log.")
                                  : QStringLiteral("No Docker container was found for this job."));
        connect(logButton, &QPushButton::clicked, this, [this, manifestPath = row.manifestPath]() {
            showProcessingJobLog(manifestPath);
        });
        table->setCellWidget(rowIndex, 6, logButton);
    }
}

void EditorWindow::showProcessingJobLog(const QString& manifestPath)
{
    QJsonObject manifest;
    QString error;
    if (!jcut::jobs::readManifest(manifestPath, &manifest, &error)) {
        QMessageBox::warning(this,
                             QStringLiteral("Open Job Log"),
                             QStringLiteral("Could not read job manifest:\n%1").arg(error));
        return;
    }

    QString dockerError;
    const QVector<jcut::jobs::DockerContainerInfo> containers =
        jcut::jobs::listDockerContainers(&dockerError);
    const jcut::jobs::DockerContainerInfo* container =
        jcut::jobs::findDockerContainerForManifest(manifest, containers);
    if (!container) {
        QMessageBox::warning(this,
                             QStringLiteral("Open Job Log"),
                             dockerError.isEmpty()
                                 ? QStringLiteral("No Docker container was found for this job.")
                                 : QStringLiteral("No Docker container was found for this job.\n\nDocker: %1")
                                       .arg(dockerError));
        return;
    }

    const QString identifier = jcut::jobs::dockerContainerIdentifier(*container);
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Job Log  %1").arg(identifier));
    dialog->resize(960, 620);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* title = new QLabel(
        QStringLiteral("%1\n%2")
            .arg(QDir::toNativeSeparators(manifestPath),
                 QStringLiteral("docker logs %1%2")
                     .arg(jcut::jobs::dockerContainerIsRunning(*container)
                              ? QStringLiteral("-f ")
                              : QString(),
                          identifier)),
        dialog);
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);

    auto* output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    layout->addWidget(output, 1);

    auto* autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(autoScrollBox);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    auto* process = new QProcess(dialog);
    process->setProcessChannelMode(QProcess::MergedChannels);
    const QString docker = QStandardPaths::findExecutable(QStringLiteral("docker"));
    if (docker.isEmpty()) {
        output->setPlainText(QStringLiteral("[process error] docker was not found in PATH\n"));
        process->deleteLater();
    } else {
        QStringList args{QStringLiteral("logs"), QStringLiteral("--tail=200")};
        if (jcut::jobs::dockerContainerIsRunning(*container)) {
            args << QStringLiteral("-f");
        }
        args << identifier;
        process->setProgram(docker);
        process->setArguments(args);
        const auto appendOutput = [output, autoScrollBox](QString text) {
            if (text.isEmpty()) {
                return;
            }
            text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            if (autoScrollBox->isChecked()) {
                output->moveCursor(QTextCursor::End);
            }
            output->insertPlainText(text);
            if (autoScrollBox->isChecked()) {
                output->moveCursor(QTextCursor::End);
            }
        };
        connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]() {
            appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput()));
        });
        connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError processError) {
            appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(processError)));
        });
        connect(process,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                dialog,
                [appendOutput](int exitCode, QProcess::ExitStatus exitStatus) {
                    appendOutput(QStringLiteral("\n[log stream finished] exitCode=%1 status=%2\n")
                                     .arg(exitCode)
                                     .arg(exitStatus == QProcess::NormalExit
                                              ? QStringLiteral("normal")
                                              : QStringLiteral("crashed")));
                });
        process->start();
    }

    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [process](int) {
        if (process && process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    });
    dialog->show();
}

void EditorWindow::refreshCurrentInspectorTab()
{
    QElapsedTimer timer;
    timer.start();
    if (!m_inspectorPane || !m_inspectorPane->tabs()) {
        return;
    }
    const int index = m_inspectorPane->tabs()->currentIndex();
    if (index < 0) {
        return;
    }
    refreshInspectorTabByName(m_inspectorPane->tabs()->tabText(index));
    const qint64 elapsedMs = timer.elapsed();
    m_lastInspectorRefreshDurationMs.store(elapsedMs);
    qint64 maxDuration = m_maxInspectorRefreshDurationMs.load();
    while (elapsedMs > maxDuration &&
           !m_maxInspectorRefreshDurationMs.compare_exchange_weak(maxDuration, elapsedMs)) {
    }
    if (elapsedMs >= m_slowSeekWarnThresholdMs) {
        m_inspectorRefreshSlowCount.fetch_add(1);
    }
}

void EditorWindow::scheduleDeferredInspectorRefresh(int delayMs)
{
    const int normalizedDelayMs = qBound(0, delayMs, 1000);
    m_deferredInspectorRefreshTimer.start(normalizedDelayMs);
}

void EditorWindow::syncAudioTabTimelineWaveforms()
{
    if (!m_timeline) {
        return;
    }

    bool audioTabActive = false;
    if (m_inspectorPane && m_inspectorPane->tabs()) {
        QTabWidget* tabs = m_inspectorPane->tabs();
        const int index = tabs->currentIndex();
        audioTabActive =
            index >= 0 &&
            tabs->tabText(index).compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0;
    }
    m_timeline->setAudioTabWaveformsVisible(audioTabActive);
}

void EditorWindow::refreshAudioInspectorViews()
{
    if (!m_audioCurrentSpeakerTitleLabel || !m_audioCurrentSpeakerDetailsLabel || !m_timeline) {
        return;
    }

    if (m_audioShowWaveformCheckBox) {
        const int selectedTrackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack* selectedTrack = m_timeline->selectedTrack();
        int audioClipCount = 0;
        if (selectedTrackIndex >= 0) {
            for (const TimelineClip& timelineClip : m_timeline->clips()) {
                if (timelineClip.trackIndex == selectedTrackIndex && timelineClip.hasAudio) {
                    ++audioClipCount;
                }
            }
        }
        const bool hasAudioTrack =
            selectedTrack && selectedTrackIndex >= 0 && m_timeline->trackHasAudioClips(selectedTrackIndex);
        QSignalBlocker block(m_audioShowWaveformCheckBox);
        m_audioShowWaveformCheckBox->setChecked(
            selectedTrack ? selectedTrack->audioWaveformVisible : m_audioWaveformVisible);
        m_audioShowWaveformCheckBox->setEnabled(hasAudioTrack);
        m_audioShowWaveformCheckBox->setToolTip(
            hasAudioTrack
                ? QStringLiteral("Show timeline waveforms for the selected track while the Audio tab is active.")
                : QStringLiteral("Select a track with audio clips to control its timeline waveform overlay."));
        if (m_inspectorPane && m_inspectorPane->audioTrackTitleLabel() &&
            m_inspectorPane->audioTrackDetailsLabel()) {
            if (selectedTrack && selectedTrackIndex >= 0) {
                const QString trackName = selectedTrack->name.trimmed().isEmpty()
                    ? QStringLiteral("Track %1").arg(selectedTrackIndex + 1)
                    : selectedTrack->name.trimmed();
                m_inspectorPane->audioTrackTitleLabel()->setText(
                    QStringLiteral("%1").arg(trackName));
                m_inspectorPane->audioTrackDetailsLabel()->setText(
                    QStringLiteral("%1 audio clip%2 • waveform %3")
                        .arg(audioClipCount)
                        .arg(audioClipCount == 1 ? QString() : QStringLiteral("s"))
                        .arg(selectedTrack->audioWaveformVisible
                                 ? QStringLiteral("shown")
                                 : QStringLiteral("hidden")));
            } else {
                m_inspectorPane->audioTrackTitleLabel()->setText(QStringLiteral("No track selected"));
                m_inspectorPane->audioTrackDetailsLabel()->setText(QStringLiteral("0 audio clips"));
            }
        }
    }

    const TimelineClip *clip = m_timeline->selectedClip();
    if (!clip || !(clip->mediaType == ClipMediaType::Audio || clip->hasAudio)) {
        m_audioCurrentSpeakerTitleLabel->setText(QStringLiteral("No audio clip selected"));
        m_audioCurrentSpeakerDetailsLabel->setText(
            QStringLiteral("Select an audio-backed clip to inspect the speaker at the current playhead."));
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClip(*clip);
    if (transcriptPath.trimmed().isEmpty()) {
        m_audioCurrentSpeakerTitleLabel->setText(QStringLiteral("No speaker information"));
        m_audioCurrentSpeakerDetailsLabel->setText(
            QStringLiteral("The selected clip does not have an active transcript with speaker metadata."));
        return;
    }

    const int64_t clipStartSample = clipTimelineStartSamples(*clip);
    const int64_t clipEndSample = clipStartSample + frameToSamples(clip->durationFrames);
    if (m_transportTimelineSample < clipStartSample || m_transportTimelineSample >= clipEndSample) {
        m_audioCurrentSpeakerTitleLabel->setText(QStringLiteral("Playhead outside selected clip"));
        m_audioCurrentSpeakerDetailsLabel->setText(
            QStringLiteral("Move the playhead into the selected clip to inspect the active speaker."));
        return;
    }

    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    if (!runtimeDocument || runtimeDocument->sections.isEmpty()) {
        m_audioCurrentSpeakerTitleLabel->setText(QStringLiteral("No speaker information"));
        m_audioCurrentSpeakerDetailsLabel->setText(
            QStringLiteral("The active transcript did not provide any speaker-tagged transcript sections."));
        return;
    }

    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
        *clip,
        m_transportTimelineSample,
        m_timeline->renderSyncMarkers());
    const QString speakerTitle =
        transcriptSpeakerTitleForSourceFrame(
            transcriptPath,
            runtimeDocument->sections,
            sourceFrame,
            TranscriptOverlayTiming{m_transcriptPrependMs, m_transcriptPostpendMs, m_transcriptOffsetMs}).trimmed();
    if (speakerTitle.isEmpty()) {
        m_audioCurrentSpeakerTitleLabel->setText(QStringLiteral("No current speaker"));
        m_audioCurrentSpeakerDetailsLabel->setText(
            QStringLiteral("No speaker-tagged transcript segment is active at the current playhead."));
        return;
    }

    const SpeakerProfile profile =
        transcriptSpeakerProfileForSourceFrame(
            transcriptPath,
            runtimeDocument->sections,
            sourceFrame,
            TranscriptOverlayTiming{m_transcriptPrependMs, m_transcriptPostpendMs, m_transcriptOffsetMs});
    m_audioCurrentSpeakerTitleLabel->setText(speakerTitle);

    QStringList details;
    if (!profile.description.trimmed().isEmpty()) {
        details.push_back(profile.description.trimmed());
    }
    if (!profile.speakerId.trimmed().isEmpty() &&
        profile.speakerId.trimmed().compare(speakerTitle, Qt::CaseSensitive) != 0) {
        details.push_back(QStringLiteral("Speaker ID: %1").arg(profile.speakerId.trimmed()));
    }
    if (details.isEmpty()) {
        details.push_back(QStringLiteral("Speaker metadata is available, but no description is set."));
    }
    m_audioCurrentSpeakerDetailsLabel->setText(details.join(QStringLiteral("\n")));
}

void EditorWindow::refreshTimelineStructureInspectorViews()
{
    if (m_clipsTab) {
        m_clipsTab->refresh();
    }
    if (m_propertiesTab) {
        m_propertiesTab->refresh();
    }
    if (m_tracksTab) {
        m_tracksTab->refresh();
    }
    if (m_historyTab) {
        m_historyTab->refresh();
    }
    refreshCurrentInspectorTab();
}

void EditorWindow::refreshTimelineSelectionInspectorViews()
{
    if (m_transcriptTab) {
        m_transcriptTab->refresh();
    }
    if (m_speakerTranscriptTab) {
        m_speakerTranscriptTab->refresh();
    }
    if (m_outputTab) {
        m_outputTab->refresh();
    }
    if (m_profileTab) {
        m_profileTab->refresh();
    }
    if (m_gradingTab) {
        m_gradingTab->refresh();
    }
    if (m_effectsTab) {
        m_effectsTab->refresh();
    }
    if (m_maskTab) {
        m_maskTab->refresh();
    }
    if (m_titlesTab) {
        m_titlesTab->refresh();
    }
    if (m_videoKeyframeTab) {
        m_videoKeyframeTab->refresh();
    }
    refreshCurrentInspectorTab();
}

void EditorWindow::refreshPreviewTransformInspectorViews()
{
    if (m_videoKeyframeTab) {
        m_videoKeyframeTab->refresh();
    }
    if (m_titlesTab) {
        m_titlesTab->refresh();
    }
    if (m_transcriptTab) {
        m_transcriptTab->refresh();
    }
    if (m_speakerTranscriptTab) {
        m_speakerTranscriptTab->refresh();
    }
    refreshCurrentInspectorTab();
}

void EditorWindow::refreshTranscriptDerivedInspectorViews(bool includeHistoryTab)
{
    if (m_transcriptTab) {
        m_transcriptTab->refresh();
    }
    if (m_speakerTranscriptTab) {
        m_speakerTranscriptTab->refresh();
    }
    if (m_speakersTab) {
        m_speakersTab->refresh();
    }
    if (includeHistoryTab && m_historyTab) {
        m_historyTab->refresh();
    }
}

void EditorWindow::refreshSpeechFilterInspectorViews()
{
    if (m_transcriptTab) {
        m_transcriptTab->refresh();
    }
    if (m_speakerTranscriptTab) {
        m_speakerTranscriptTab->refresh();
    }
}

void EditorWindow::createOutputTab()
{
    m_outputTab = std::make_unique<OutputTab>(
        OutputTab::Widgets{
            m_outputWidthSpin, m_outputHeightSpin, m_outputFpsSpin,
            m_exportStartSpin, m_exportEndSpin,
            m_outputFormatCombo, m_backgroundFillEffectCombo, m_backgroundFillOpacitySpin,
            m_backgroundFillBrightnessSpin, m_backgroundFillSaturationSpin,
            m_backgroundFillEdgePixelsSlider, m_backgroundFillEdgeProgressiveCheckBox,
            m_backgroundFillEdgePowerSpin,
            m_outputRangeSummaryLabel, m_renderUseProxiesCheckBox,
            m_outputPlaybackCacheFallbackCheckBox, m_outputLeadPrefetchEnabledCheckBox,
            m_outputLeadPrefetchCountSpin, m_outputPlaybackWindowAheadSpin, m_outputVisibleQueueReserveSpin,
            m_outputPrefetchMaxQueueDepthSpin, m_outputPrefetchMaxInflightSpin,
            m_outputPrefetchMaxPerTickSpin, m_outputPrefetchSkipVisiblePendingThresholdSpin,
            m_outputDecoderLaneCountSpin, m_outputDecodeModeCombo,
            m_outputDeterministicPipelineCheckBox, m_outputResetPipelineDefaultsButton,
            m_autosaveIntervalMinutesSpin, m_autosaveMaxBackupsSpin,
            m_createImageSequenceCheckBox, m_imageSequenceFormatCombo, m_renderButton},
        OutputTab::Dependencies{
            [this]() { return m_timeline != nullptr; },
            [this]() { return m_timeline && !m_timeline->clips().isEmpty(); },
            [this]() -> int64_t { return m_timeline ? m_timeline->totalFrames() : 0; },
            [this]() -> int64_t { return m_timeline ? m_timeline->exportStartFrame() : 0; },
            [this]() -> int64_t { return m_timeline ? m_timeline->exportEndFrame() : 0; },
            [this]() -> double { return m_exportPlaybackSpeed; },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t startFrame, int64_t endFrame) { if (m_timeline) m_timeline->setExportRange(startFrame, endFrame); },
            [this](const QSize& size) { if (m_preview) m_preview->setOutputSize(size); },
            [this]() { setPlaybackActive(false); },
            [this](const jcut::render::RenderRequestCore& request) {
                renderTimelineFromOutputRequestCore(request);
            },
            [this]() { return m_lastRenderOutputPath; },
            [this](const QString& path) {
                m_lastRenderOutputPath = path;
                scheduleSaveState();
            },
            [this]() { return m_autosaveIntervalMinutes; },
            [this](int minutes) {
                m_autosaveIntervalMinutes = qBound(1, minutes, 120);
                m_autosaveTimer.setInterval(m_autosaveIntervalMinutes * 60 * 1000);
            },
            [this]() { return m_autosaveMaxBackups; },
            [this](int maxBackups) {
                m_autosaveMaxBackups = qBound(1, maxBackups, 200);
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); }});
    m_outputTab->wire();
    if (m_renderUseProxiesCheckBox) {
        connect(m_renderUseProxiesCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setUseProxyMedia(checked);
                if (m_timeline) {
                    m_preview->setTimelineClips(m_timeline->clips());
                }
            }
        });
    }
}

void EditorWindow::createProfileTab()
{
    m_profileTab = std::make_unique<ProfileTab>(
        ProfileTab::Widgets{
            m_profileSummaryTable,
            m_profileBenchmarkButton,
            m_inspectorPane ? m_inspectorPane->profileH26xThreadingModeCombo() : nullptr},
        ProfileTab::Dependencies{
            [this]() { return profilingSnapshot(); },
            [this](TimelineClip* clipOut) { return profileBenchmarkClip(clipOut); },
            [this](const TimelineClip& clip) { return playbackMediaPathForClip(clip); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("System")); },
            [this]() { scheduleSaveState(); }});
    m_profileTab->wire();
}

void EditorWindow::createPipelineTab()
{
    m_pipelineTab = std::make_unique<PipelineTab>(
        PipelineTab::Widgets{
            m_inspectorPane ? m_inspectorPane->pipelinePreviewHost() : nullptr,
            m_inspectorPane ? m_inspectorPane->pipelineStageList() : nullptr},
        PipelineTab::Dependencies{
            [this]() {
                return m_preview && m_preview->backendName().contains(QStringLiteral("Vulkan"), Qt::CaseInsensitive);
            },
            [this]() {
                return m_preview ? m_preview->livePipelineSnapshots()
                                 : QVector<PreviewSurface::PipelineStageSnapshot>{};
            }});
}

void EditorWindow::createProjectsTab()
{
    m_projectsTab = std::make_unique<ProjectsTab>(
        ProjectsTab::Widgets{
            m_projectSectionLabel,
            m_projectPathLabel,
            m_projectsList,
            m_newProjectButton,
            m_saveProjectAsButton,
            m_renameProjectButton},
        ProjectsTab::Dependencies{
            m_projectManager.get(),
            [this](const QString& projectId) { switchToProject(projectId); },
            [this]() { createProject(); },
            [this]() { saveProjectAs(); },
            [this](const QString& projectId) { renameProject(projectId); },
        });
    m_projectsTab->wire();
}

void EditorWindow::createTranscriptTab()
{
    m_transcriptTab = std::make_unique<TranscriptTab>(
        TranscriptTab::Widgets{
            m_transcriptInspectorClipLabel, m_transcriptInspectorDetailsLabel,
            m_transcriptTable, m_transcriptOverlayEnabledCheckBox,
            m_transcriptPlacementModeCombo,
            m_inspectorPane->transcriptBackgroundVisibleCheckBox(),
            m_inspectorPane->transcriptBackgroundOpacitySpin(),
            m_inspectorPane->transcriptBackgroundCornerRadiusSpin(),
            m_inspectorPane->transcriptShadowEnabledCheckBox(),
            m_inspectorPane->transcriptShowSpeakerTitleCheckBox(),
            m_transcriptMaxLinesSpin, m_transcriptMaxCharsSpin,
            m_transcriptAutoScrollCheckBox, m_transcriptFollowCurrentWordCheckBox,
            m_transcriptOverlayXSpin, m_transcriptOverlayYSpin,
            m_inspectorPane->transcriptCenterHorizontalButton(),
            m_inspectorPane->transcriptCenterVerticalButton(),
            m_transcriptOverlayWidthSpin, m_transcriptOverlayHeightSpin,
            m_transcriptFontFamilyCombo, m_transcriptFontSizeSpin,
            m_transcriptBoldCheckBox, m_transcriptItalicCheckBox,
            m_transcriptPrependMsSpin, m_transcriptPostpendMsSpin,
            m_inspectorPane->transcriptOffsetMsSpin(),
            m_speechFilterEnabledCheckBox, m_speechFilterFadeSamplesSpin,
            m_inspectorPane->transcriptUnifiedEditModeCheckBox(),
            m_inspectorPane->transcriptSearchFilterLineEdit(),
            m_inspectorPane->transcriptSpeakerFilterCombo(),
            m_inspectorPane->transcriptScriptVersionCombo(),
            m_inspectorPane->transcriptNewVersionButton(),
            m_inspectorPane->transcriptDeleteVersionButton(),
            m_inspectorPane->transcriptExportTextButton(),
            m_inspectorPane->transcriptShowExcludedLinesCheckBox()},
        TranscriptTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Transcript")); },
            [this]() {
                if (!m_preview || !m_timeline) {
                    return;
                }
                if (const TimelineClip* clip = m_timeline->selectedClip()) {
                    m_preview->invalidateTranscriptOverlayCache(clip->filePath);
                } else {
                    m_preview->invalidateTranscriptOverlayCache();
                }
                m_preview->setTimelineTracks(m_timeline->tracks());
                m_preview->setTimelineClips(m_timeline->clips());
            },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            [this]() { return playbackActive(); },
            [this]() {
                const QString format = m_outputFormatCombo
                    ? m_outputFormatCombo->currentData().toString()
                    : QStringLiteral("mp4");
                return format.trimmed().isEmpty() ? QStringLiteral("mp4") : format.trimmed();
            },
            [this]() {
                return std::isfinite(m_exportPlaybackSpeed) && m_exportPlaybackSpeed > 0.001
                    ? m_exportPlaybackSpeed
                    : 1.0;
            }});
    m_transcriptTab->wire();
    m_transcriptTab->setManualSelectionHoldMs(m_transcriptManualSelectionHoldMs);

    connect(m_transcriptTab.get(), &TranscriptTab::transcriptDocumentChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        invalidatePlaybackRangeCaches();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) {
            if (const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr) {
                m_preview->invalidateTranscriptOverlayCache(clip->filePath);
            } else {
                m_preview->invalidateTranscriptOverlayCache();
            }
        }
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        scheduleTranscriptNormalizeRangeRefresh(50);
        refreshTranscriptDerivedInspectorViews(true);
    });

    connect(m_transcriptTab.get(), &TranscriptTab::speechFilterParametersChanged, this, [this]() {
        m_speechFilterEnabled = m_transcriptTab->speechFilterEnabled();
        m_transcriptPrependMs = m_transcriptPrependMsSpin ? qMax(0, m_transcriptPrependMsSpin->value()) : 150;
        m_transcriptPostpendMs = m_transcriptPostpendMsSpin ? qMax(0, m_transcriptPostpendMsSpin->value()) : 70;
        m_transcriptOffsetMs =
            m_inspectorPane->transcriptOffsetMsSpin() ? m_inspectorPane->transcriptOffsetMsSpin()->value() : 0;
        if (m_preview) {
            m_preview->setTranscriptOverlayTimingPaddingMs(
                m_transcriptPrependMs, m_transcriptPostpendMs, m_transcriptOffsetMs);
        }
        m_speechFilterFadeSamples = m_transcriptTab->speechFilterFadeSamples();
        m_transcriptEngine.invalidateCache();
        invalidatePlaybackRangeCaches();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        scheduleTranscriptNormalizeRangeRefresh(50);
        if (m_timeline && m_preview) {
            const bool duringPlayback = m_playbackTimer.isActive();
            setCurrentPlaybackSample(m_transportTimelineSample, duringPlayback, duringPlayback);
        }
        refreshSpeechFilterInspectorViews();
    });
}

void EditorWindow::createSpeakersTab()
{
    m_speakerTranscriptTable = m_inspectorPane->speakerTranscriptTable();
    TranscriptTab::Widgets speakerTranscriptWidgets;
    speakerTranscriptWidgets.transcriptTable = m_speakerTranscriptTable;
    m_speakerTranscriptTab = std::make_unique<TranscriptTab>(
        speakerTranscriptWidgets,
        TranscriptTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline && m_timeline->updateClipById(id, updater);
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Speakers")); },
            [this]() {
                if (m_preview && m_timeline) {
                    m_preview->setTimelineTracks(m_timeline->tracks());
                    m_preview->setTimelineClips(m_timeline->clips());
                }
            },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            [this]() { return playbackActive(); }});
    m_speakerTranscriptTab->wire();
    m_speakerTranscriptTab->setManualSelectionHoldMs(m_transcriptManualSelectionHoldMs);
    connect(m_speakerTranscriptTab.get(), &TranscriptTab::transcriptDocumentChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        invalidatePlaybackRangeCaches();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) {
            if (const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr) {
                m_preview->invalidateTranscriptOverlayCache(clip->filePath);
            } else {
                m_preview->invalidateTranscriptOverlayCache();
            }
            m_preview->setExportRanges(ranges);
        }
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        scheduleTranscriptNormalizeRangeRefresh(50);
        refreshTranscriptDerivedInspectorViews(true);
    });

    m_speakersTab = std::make_unique<SpeakersTab>(
        SpeakersTab::Widgets{
            m_inspectorPane->speakersInspectorClipLabel(),
            m_inspectorPane->speakersInspectorDetailsLabel(),
            m_inspectorPane->speakersTable(),
            m_inspectorPane->speakerHideUnidentifiedCheckBox(),
            m_inspectorPane->speakerShowContiguousSectionsCheckBox(),
            m_inspectorPane->speakerApplyTrackToAllMatchingSectionsCheckBox(),
            m_inspectorPane->speakerSectionMinimumWordsSpin(),
            m_inspectorPane->speakerExportLongSectionsButton(),
            m_inspectorPane->speakerShowCurrentSpeakerNameCheckBox(),
            m_inspectorPane->speakerShowCurrentSpeakerOrganizationCheckBox(),
            m_inspectorPane->speakerSectionsTable(),
            m_inspectorPane->selectedSpeakerIdLabel(),
            m_inspectorPane->selectedSpeakerNameEdit(),
            m_inspectorPane->selectedSpeakerOrganizationEdit(),
            m_inspectorPane->selectedSpeakerFaceDetectionsList(),
            m_inspectorPane->speakerPlayheadFaceDetectionsList(),
            m_inspectorPane->speakerShowPlayheadFaceDetectionsCheckBox(),
            m_inspectorPane->selectedSpeakerPreviousSentenceButton(),
            m_inspectorPane->selectedSpeakerNextSentenceButton(),
            m_inspectorPane->selectedSpeakerNextSectionButton(),
            m_inspectorPane->selectedSpeakerRandomSentenceButton(),
            m_inspectorPane->speakerCurrentSentenceLabel(),
            m_inspectorPane->speakerRunAutoTrackButton(),
            m_inspectorPane->speakerViewFacestreamButton(),
            m_inspectorPane->speakerFacestreamSettingsButton(),
            m_inspectorPane->speakerRefreshTrackAvatarsButton(),
            m_inspectorPane->speakerEnableTrackingButton(),
            m_inspectorPane->speakerDisableTrackingButton(),
            m_inspectorPane->speakerDeletePointstreamButton(),
            m_inspectorPane->speakerGuideButton(),
            m_inspectorPane->speakerPrecropFacesButton(),
            m_inspectorPane->speakerAiFindNamesButton(),
            m_inspectorPane->speakerAiFindOrganizationsButton(),
            m_inspectorPane->speakerAiCleanAssignmentsButton(),
            m_inspectorPane->speakerTrackingStatusLabel(),
            m_inspectorPane->speakerFramingTargetXSpin(),
            m_inspectorPane->speakerFramingTargetYSpin(),
            m_inspectorPane->speakerFramingTargetBoxSpin(),
            m_inspectorPane->speakerSectionRotationSpin(),
            m_inspectorPane->speakerFramingZoomEnabledCheckBox(),
            m_inspectorPane->speakerFramingCenterSmoothingFramesSpin(),
            m_inspectorPane->speakerFramingZoomSmoothingFramesSpin(),
            m_inspectorPane->speakerFramingSmoothingModeCombo(),
            m_inspectorPane->speakerFramingCenterSmoothingStrengthSpin(),
            m_inspectorPane->speakerFramingZoomSmoothingStrengthSpin(),
            m_inspectorPane->speakerFramingGapHoldFramesSpin(),
            m_inspectorPane->speakerApplyFramingToClipCheckBox(),
            m_inspectorPane->speakerFramingEnabledKeyframeTable(),
            m_inspectorPane->speakerClipFramingStatusLabel(),
            m_inspectorPane->speakerRefsChipLabel(),
            m_inspectorPane->speakerPointstreamChipLabel(),
            m_inspectorPane->speakerTrackingChipButton(),
            m_inspectorPane->speakerStabilizeChipButton(),
            m_inspectorPane->speakerShowFaceDetectionsBoxesCheckBox(),
            m_inspectorPane->speakerFaceDetectionsTable(),
            m_inspectorPane->speakerFaceDetectionsDetailsEdit(),
            m_inspectorPane->speakerDetectionsAvailableCheckBox(),
            m_inspectorPane->speakerTracksAvailableCheckBox(),
            m_inspectorPane->speakerRawDetectionTable(),
            m_inspectorPane->speakerRawDetectionDetailsEdit()},
        SpeakersTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Speakers")); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this](int64_t frame) { setCurrentFrame(frame); },
            [this]() -> QVector<RenderSyncMarker> {
                return m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>{};
            },
            [this](const QString& clipId, const std::function<void(TimelineClip&)>& updater) -> bool {
                return m_timeline && m_timeline->updateClipById(clipId, updater);
            },
            [this](const QString& clipId) -> bool {
                if (!m_timeline || clipId.trimmed().isEmpty()) {
                    return false;
                }
                m_timeline->setSelectedClipId(clipId);
                return m_timeline->selectedClipId() == clipId;
            },
            [this]() -> QSize {
                return m_preview ? m_preview->outputSize() : QSize(1080, 1920);
            },
            [this]() {
                if (!m_preview || !m_timeline) {
                    return;
                }
                m_preview->setTimelineClips(m_timeline->clips());
                m_preview->asWidget()->update();
            },
            [this](const QSet<int>& trackIds) {
                if (m_preview) {
                    m_preview->setSelectedSpeakerAssignedFaceTrackIds(trackIds);
                }
            },
            {},
            [this]() -> bool { return m_playbackTimer.isActive(); },
            [this](QString* errorOut) -> bool {
                refreshAiIntegrationState();
                if (!m_featureAiSpeakerCleanup) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI speaker actions disabled by feature flag.");
                    }
                    return false;
                }
                if (!m_aiIntegrationEnabled) {
                    if (errorOut) {
                        *errorOut = m_aiIntegrationStatus;
                    }
                    return false;
                }
                if (m_aiAuthToken.trimmed().isEmpty()) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI login required. Use top-right Log In.");
                    }
                    return false;
                }
                return true;
            },
            [this](const QStringList& speakerIds) {
                exportVideoForSpeakersOnSelectedClip(speakerIds);
            },
            [this](const QString& speakerId,
                   int64_t startFrame,
                   int64_t endFrame,
                   const QString& snippet,
                   const QString& speakerDisplayName,
                   int sectionOrdinal) {
                exportVideoForSpeakerSectionOnSelectedClip(
                    speakerId, startFrame, endFrame, snippet, speakerDisplayName, sectionOrdinal);
            },
            [this](const QVector<SpeakerSectionExportItem>& sections) {
                exportVideoForSpeakerSectionsOnSelectedClip(sections);
            },
            [this](bool suppressed) {
                if (m_audioEngine) {
                    m_audioEngine->setBackgroundDecodeSuppressed(suppressed);
                }
            }});
    m_speakersTab->wire();
    if (m_inspectorPane) {
        if (SpeakersTable* speakersTable =
                qobject_cast<SpeakersTable*>(m_inspectorPane->speakersTable())) {
            connect(speakersTable, &SpeakersTable::columnVisibilityChanged, this, [this]() {
                scheduleSaveState();
            });
        }
    }

    auto syncFaceDetectionsAssignmentMode = [this]() {
        if (!m_preview) {
            return;
        }
        bool speakersTabActive = false;
        if (m_inspectorPane && m_inspectorPane->tabs()) {
            QTabWidget* tabs = m_inspectorPane->tabs();
            const int index = tabs->currentIndex();
            speakersTabActive =
                index >= 0 &&
                tabs->tabText(index).compare(QStringLiteral("Speakers"), Qt::CaseInsensitive) == 0;
        }
        m_preview->setFaceDetectionsAssignmentInteractionEnabled(speakersTabActive);
    };
    if (m_inspectorPane && m_inspectorPane->tabs()) {
        connect(m_inspectorPane->tabs(), &QTabWidget::currentChanged, this,
                [syncFaceDetectionsAssignmentMode](int) { syncFaceDetectionsAssignmentMode(); });
    }
    if (m_inspectorPane && m_inspectorPane->speakersSubtabs()) {
        connect(m_inspectorPane->speakersSubtabs(), &QTabWidget::currentChanged, this,
                [syncFaceDetectionsAssignmentMode](int) { syncFaceDetectionsAssignmentMode(); });
    }
    syncFaceDetectionsAssignmentMode();

    connect(m_speakersTab.get(), &SpeakersTab::transcriptDocumentChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        invalidatePlaybackRangeCaches();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) {
            if (const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr) {
                m_preview->invalidateTranscriptOverlayCache(clip->filePath);
            } else {
                m_preview->invalidateTranscriptOverlayCache();
            }
        }
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        scheduleTranscriptNormalizeRangeRefresh(50);
        refreshTranscriptDerivedInspectorViews(true);
    });
}

void EditorWindow::createGradingTab()
{
    m_gradingTab = std::make_unique<GradingTab>(
        GradingTab::Widgets{
            m_gradingPathLabel, m_brightnessSpin, m_contrastSpin,
            m_saturationSpin,
            // Shadows/Midtones/Highlights
            m_shadowsRSpin, m_shadowsGSpin, m_shadowsBSpin,
            m_midtonesRSpin, m_midtonesGSpin, m_midtonesBSpin,
            m_highlightsRSpin, m_highlightsGSpin, m_highlightsBSpin,
            m_gradingKeyframeTable,
            m_gradingAutoScrollCheckBox, m_gradingFollowCurrentCheckBox,
            m_gradingKeyAtPlayheadButton,
            m_inspectorPane->gradingAutoOpposeButton(),
            m_inspectorPane->gradingCurveChannelCombo(),
            m_inspectorPane->gradingCurveThreePointLockCheckBox(),
            m_inspectorPane->gradingCurveSmoothingCheckBox(),
            m_inspectorPane->gradingHistogramWidget()},
        GradingTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Grade")); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {},
            [this]() -> QImage {
                if (!m_preview || !m_timeline) {
                    return QImage();
                }
                const QString clipId = m_timeline->selectedClipId();
                if (clipId.isEmpty()) {
                    return QImage();
                }
                return m_preview->latestPresentedFrameImageForClip(clipId);
            },
            [this]() -> bool {
                return !playbackActive();
            }});
    m_gradingTab->wire();
}

void EditorWindow::createOpacityTab()
{
    m_opacityTab = std::make_unique<OpacityTab>(
        OpacityTab::Widgets{
            m_inspectorPane->opacityPathLabel(),
            m_opacitySpin,
            m_inspectorPane->opacityKeyframeTable(),
            m_inspectorPane->opacityAutoScrollCheckBox(),
            m_inspectorPane->opacityFollowCurrentCheckBox(),
            m_inspectorPane->opacityKeyAtPlayheadButton(),
            m_inspectorPane->opacityFadeInButton(),
            m_inspectorPane->opacityFadeOutButton(),
            m_inspectorPane->opacityFadeDurationSpin()},
        OpacityTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Opacity")); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_opacityTab->wire();
}

void EditorWindow::createEffectsTab()
{
    m_effectsTab = std::make_unique<EffectsTab>(
        EffectsTab::Widgets{
            m_inspectorPane->effectsPathLabel(),
            m_inspectorPane->maskFeatherSpin(),
            m_inspectorPane->maskFeatherGammaSpin(),
            m_inspectorPane->maskFeatherEnabledCheck(),
            nullptr},
        EffectsTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Effects")); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this](const TimelineClip& clip) { return clipHasAlpha(clip); }});
    m_effectsTab->wire();
}

void EditorWindow::createMaskTab()
{
    m_maskTab = std::make_unique<MaskTab>(
        MaskTab::Widgets{
            m_inspectorPane->maskClipLabel(),
            m_inspectorPane->maskEnabledCheck(),
            m_inspectorPane->maskFramesDirEdit(),
            m_inspectorPane->maskBrowseButton(),
            m_inspectorPane->maskShapeFeatherSpin(),
            m_inspectorPane->maskDilateSpin(),
            m_inspectorPane->maskErodeSpin(),
            m_inspectorPane->maskBlurSpin(),
            m_inspectorPane->maskInvertCheck(),
            m_inspectorPane->maskOpacitySpin(),
            m_inspectorPane->maskGradeEnabledCheck(),
            m_inspectorPane->maskGradeBrightnessSpin(),
            m_inspectorPane->maskGradeContrastSpin(),
            m_inspectorPane->maskGradeSaturationSpin(),
            m_inspectorPane->maskShadowEnabledCheck(),
            m_inspectorPane->maskShadowRadiusSpin(),
            m_inspectorPane->maskShadowOffsetXSpin(),
            m_inspectorPane->maskShadowOffsetYSpin(),
            m_inspectorPane->maskShadowOpacitySpin()},
        MaskTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Masks")); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this](QWidget* parent, const QString& currentPath) {
                return QFileDialog::getExistingDirectory(
                    parent ? parent : this,
                    QStringLiteral("Choose Mask Frames Directory"),
                    currentPath.trimmed().isEmpty() ? QDir::homePath() : currentPath,
                    QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
            }});
    m_maskTab->wire();
}

void EditorWindow::createCorrectionsTab()
{
    m_correctionsTab = std::make_unique<CorrectionsTab>(
        CorrectionsTab::Widgets{
            m_inspectorPane->correctionsClipLabel(),
            m_inspectorPane->correctionsStatusLabel(),
            m_inspectorPane->correctionsEnabledCheck(),
            m_inspectorPane->correctionsPolygonTable(),
            m_inspectorPane->correctionsVertexTable(),
            m_inspectorPane->correctionsDrawModeCheck(),
            m_inspectorPane->correctionsDrawPolygonButton(),
            m_inspectorPane->correctionsClosePolygonButton(),
            m_inspectorPane->correctionsCancelDraftButton(),
            m_inspectorPane->correctionsDeleteLastButton(),
            m_inspectorPane->correctionsClearAllButton()},
        CorrectionsTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline ? m_timeline->updateClipById(id, updater) : false;
            },
            [this]() {
                if (m_preview && m_timeline) {
                    m_preview->setTimelineTracks(m_timeline->tracks());
                    m_preview->setTimelineClips(m_timeline->clips());
                }
            },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Corrections")); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { return m_correctionsEnabled; },
            [this](bool enabled) {
                m_correctionsEnabled = enabled;
                if (m_preview) {
                    m_preview->setCorrectionsEnabled(enabled);
                    if (m_timeline) {
                        m_preview->setTimelineClips(m_timeline->clips());
                    }
                }
                scheduleSaveState();
                pushHistorySnapshot();
            },
            [this](bool enabled) { if (m_preview) m_preview->setCorrectionDrawMode(enabled); },
            [this](int polygonIndex) {
                if (m_preview) {
                    m_preview->setSelectedCorrectionPolygon(polygonIndex);
                }
            },
            [this](const QVector<QPointF>& points) {
                if (m_preview) {
                    m_preview->setCorrectionDraftPoints(points);
                }
            },
            [this](TimelineWidget::ToolMode mode) { if (m_timeline) m_timeline->setToolMode(mode); },
            [this]() -> TimelineWidget::ToolMode { return m_timeline ? m_timeline->toolMode() : TimelineWidget::ToolMode::Select; }});
    m_correctionsTab->wire();
}

void EditorWindow::createTitlesTab()
{
    m_titlesTab = std::make_unique<TitlesTab>(
        TitlesTab::Widgets{
            m_inspectorPane->titlesInspectorClipLabel(),
            m_inspectorPane->titlesInspectorDetailsLabel(),
            m_inspectorPane->titleKeyframeTable(),
            m_inspectorPane->titleTextEdit(),
            m_inspectorPane->titleXSpin(),
            m_inspectorPane->titleYSpin(),
            m_inspectorPane->titleFontSizeSpin(),
            m_inspectorPane->titleOpacitySpin(),
            m_inspectorPane->titleFontCombo(),
            m_inspectorPane->titleBoldCheck(),
            m_inspectorPane->titleItalicCheck(),
            m_inspectorPane->titleColorButton(),
            m_inspectorPane->titleShadowEnabledCheck(),
            m_inspectorPane->titleShadowColorButton(),
            m_inspectorPane->titleShadowOpacitySpin(),
            m_inspectorPane->titleShadowOffsetXSpin(),
            m_inspectorPane->titleShadowOffsetYSpin(),
            m_inspectorPane->titleWindowEnabledCheck(),
            m_inspectorPane->titleWindowColorButton(),
            m_inspectorPane->titleWindowOpacitySpin(),
            m_inspectorPane->titleWindowPaddingSpin(),
            m_inspectorPane->titleWindowFrameEnabledCheck(),
            m_inspectorPane->titleWindowFrameColorButton(),
            m_inspectorPane->titleWindowFrameOpacitySpin(),
            m_inspectorPane->titleWindowFrameWidthSpin(),
            m_inspectorPane->titleWindowFrameGapSpin(),
            m_inspectorPane->titleAutoScrollCheck(),
            m_inspectorPane->addTitleKeyframeButton(),
            m_inspectorPane->removeTitleKeyframeButton(),
            m_inspectorPane->titleCenterHorizontalButton(),
            m_inspectorPane->titleCenterVerticalButton()},
        TitlesTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Titles")); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_titlesTab->wire();
}

void EditorWindow::createVideoKeyframeTab()
{
    m_videoKeyframeTab = std::make_unique<VideoKeyframeTab>(
        VideoKeyframeTab::Widgets{
            m_keyframesInspectorClipLabel, m_keyframesInspectorDetailsLabel,
            m_videoKeyframeTable, m_videoTranslationXSpin, m_videoTranslationYSpin,
            m_videoRotationSpin, m_videoScaleXSpin, m_videoScaleYSpin,
            m_videoInterpolationCombo, m_mirrorHorizontalCheckBox,
            m_mirrorVerticalCheckBox, m_lockVideoScaleCheckBox,
            m_keyframeSpaceCheckBox, m_keyframeSkipAwareTimingCheckBox, m_keyframesAutoScrollCheckBox,
            m_keyframesFollowCurrentCheckBox, m_addVideoKeyframeButton, m_removeVideoKeyframeButton,
            m_flipHorizontalButton},
        VideoKeyframeTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Transform")); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_videoKeyframeTab->wire();
}

void EditorWindow::createClipsTab()
{
    m_clipsTab = std::make_unique<ClipsTab>(
        ClipsTab::Widgets{m_inspectorPane->clipsTable()},
        ClipsTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->clips() : QVector<TimelineClip>{}; },
            [this]() { return m_timeline ? m_timeline->tracks() : QVector<TimelineTrack>{}; },
            [this](const QString& clipId) { return m_timeline ? m_timeline->deleteClipById(clipId) : false; },
            [this](const QString& clipId) { if (m_timeline) m_timeline->setSelectedClipId(clipId); },
            [this](const QString& clipId, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline ? m_timeline->updateClipById(clipId, updater) : false;
            },
            [this](int trackIndex, const std::function<void(TimelineTrack&)>& updater) {
                return m_timeline ? m_timeline->updateTrackByIndex(trackIndex, updater) : false;
            },
            [this]() { pushHistorySnapshot(); },
            [this]() { scheduleSaveState(); },
            [this](const QString& clipId) { openSamDetectorWindow(clipId); }});
    m_clipsTab->wire();
}

void EditorWindow::createHistoryTab()
{
    m_historyTab = std::make_unique<HistoryTab>(
        HistoryTab::Widgets{m_inspectorPane->historyTable()},
        HistoryTab::Dependencies{
            [this]() -> QJsonArray { return m_historyEntries; },
            [this]() -> int { return m_historyIndex; },
            [this](int index) { restoreToHistoryIndex(index); },
            [this]() { pushHistorySnapshot(); }});
    m_historyTab->wire();
}

void EditorWindow::createSyncTab()
{
    m_syncTab = std::make_unique<SyncTab>(
        SyncTab::Widgets{
            m_syncInspectorClipLabel,
            m_syncInspectorDetailsLabel,
            m_syncTable,
            m_clearAllSyncPointsButton},
        SyncTab::Dependencies{
            [this]() -> const TimelineClip* { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() -> QVector<RenderSyncMarker> { return m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>{}; },
            [this](const QVector<RenderSyncMarker>& markers) { if (m_timeline) m_timeline->setRenderSyncMarkers(markers); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this](int64_t frame) { setCurrentFrame(frame); },
            [this](const QString& clipId) { return clipLabelForId(clipId); },
            [this](const QString& clipId) { return clipColorForId(clipId); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](int markerCount) -> bool {
                const int response = QMessageBox::question(
                    this,
                    QStringLiteral("Clear All Sync Points"),
                    QStringLiteral("Remove all %1 sync points from the timeline?").arg(markerCount),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                return response == QMessageBox::Yes;
            },
            [this]() {
                QMessageBox::information(this,
                                         QStringLiteral("Clear Sync Points"),
                                         QStringLiteral("There are no sync points to clear."));
            }});
    m_syncTab->wire();
}

void EditorWindow::createTracksTab()
{
    m_tracksTab = std::make_unique<TracksTab>(
        TracksTab::Widgets{m_inspectorPane ? m_inspectorPane->tracksTable() : nullptr},
        TracksTab::Dependencies{
            [this]() -> QVector<TimelineTrack> { return m_timeline ? m_timeline->tracks() : QVector<TimelineTrack>{}; },
            [this](int trackIndex) -> bool { return m_timeline ? m_timeline->trackHasVisualClips(trackIndex) : false; },
            [this](int trackIndex) -> TrackVisualMode { return m_timeline ? m_timeline->trackVisualMode(trackIndex) : TrackVisualMode::Enabled; },
            [this](int trackIndex) -> bool { return m_timeline ? m_timeline->trackHasAudioClips(trackIndex) : false; },
            [this](int trackIndex) -> bool { return m_timeline ? m_timeline->trackAudioEnabled(trackIndex) : false; },
            [this](int trackIndex, TrackVisualMode mode) -> bool { return m_timeline ? m_timeline->updateTrackVisualMode(trackIndex, mode) : false; },
            [this](int trackIndex, bool enabled) -> bool { return m_timeline ? m_timeline->updateTrackAudioEnabled(trackIndex, enabled) : false; },
            [this](int trackIndex, qreal gain) -> bool {
                return m_timeline ? m_timeline->updateTrackByIndex(trackIndex, [gain](TimelineTrack& track) {
                    track.audioGain = qBound<qreal>(0.0, gain, 4.0);
                }) : false;
            },
            [this](int trackIndex, bool muted) -> bool {
                return m_timeline ? m_timeline->updateTrackByIndex(trackIndex, [muted](TimelineTrack& track) {
                    track.audioMuted = muted;
                }) : false;
            },
            [this](int trackIndex, bool solo) -> bool {
                return m_timeline ? m_timeline->updateTrackByIndex(trackIndex, [solo](TimelineTrack& track) {
                    track.audioSolo = solo;
                }) : false;
            },
            [this]() { if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Tracks")); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); }});
    m_tracksTab->wire();
}

void EditorWindow::createPropertiesTab()
{
    m_propertiesTab = std::make_unique<PropertiesTab>(
        PropertiesTab::Widgets{
            m_clipInspectorClipLabel,
            m_clipProxyUsageLabel,
            m_clipPlaybackSourceLabel,
            m_clipOriginalInfoLabel,
            m_clipProxyInfoLabel,
            m_clipPlaybackRateSpin,
            m_trackInspectorLabel,
            m_trackInspectorDetailsLabel,
            m_trackNameEdit,
            m_trackHeightSpin,
            m_trackVisualModeCombo,
            m_trackAudioEnabledCheckBox,
            m_trackCrossfadeSecondsSpin,
            m_trackCrossfadeButton},
        PropertiesTab::Dependencies{
            [this]() -> const TimelineClip* { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() -> const TimelineTrack* { return m_timeline ? m_timeline->selectedTrack() : nullptr; },
            [this]() -> int { return m_timeline ? m_timeline->selectedTrackIndex() : -1; },
            [this]() -> QVector<TimelineClip> { return m_timeline ? m_timeline->clips() : QVector<TimelineClip>{}; },
            [this](const TimelineClip& clip) { return playbackProxyPathForClip(clip); },
            [this](const TimelineClip& clip) { return playbackMediaPathForClip(clip); },
            [this](const TimelineClip& clip, const MediaProbeResult* knownProbe) { return clipFileInfoSummary(clip.filePath, knownProbe); },
            [this](const QString& path) { return clipFileInfoSummary(path); },
            [this](const TimelineClip& clip) { return defaultProxyOutputPath(clip); }});
}

void EditorWindow::setupTabs()
{
    createOutputTab();
    createPipelineTab();
    createProfileTab();
    createProjectsTab();
    createTranscriptTab();
    createSpeakersTab();
    createGradingTab();
    createOpacityTab();
    createEffectsTab();
    createMaskTab();
    createCorrectionsTab();
    createTitlesTab();
    createVideoKeyframeTab();
    createClipsTab();
    createHistoryTab();
    createSyncTab();
    createTracksTab();
    createPropertiesTab();

    // Ensure correction draw mode is disabled when Corrections tab is not selected
    if (m_inspectorPane && m_inspectorPane->tabs()) {
        connect(m_inspectorPane->tabs(), &QTabWidget::currentChanged, this,
                [this](int index) {
            const QString tabName = m_inspectorPane->tabs()->tabText(index);
            syncAudioTabTimelineWaveforms();
            if (m_preview) {
                const bool isCorrectionsTab = tabName.compare(QStringLiteral("Corrections"), Qt::CaseInsensitive) == 0;
                const bool isTitlesTab = tabName.compare(QStringLiteral("Titles"), Qt::CaseInsensitive) == 0;
                if (!isCorrectionsTab && m_preview->correctionDrawMode()) {
                    m_preview->setCorrectionDrawMode(false);
                    if (m_correctionsTab) {
                        m_correctionsTab->stopDrawing();
                    }
                }
                m_preview->setTitleOverlayInteractionOnly(isTitlesTab);

            }

            refreshInspectorTabByName(tabName);
        });
        syncAudioTabTimelineWaveforms();
        m_processingJobsRefreshTimer.setInterval(2000);
        connect(&m_processingJobsRefreshTimer, &QTimer::timeout, this, [this]() {
            if (!m_inspectorPane || !m_inspectorPane->tabs()) {
                return;
            }
            const int index = m_inspectorPane->tabs()->currentIndex();
            if (index >= 0 &&
                m_inspectorPane->tabs()->tabText(index).compare(QStringLiteral("Jobs"), Qt::CaseInsensitive) == 0) {
                refreshProcessingJobsTab();
            }
        });
        m_processingJobsRefreshTimer.start();
    }
    if (m_inspectorPane && m_inspectorPane->speakersSubtabs()) {
        connect(m_inspectorPane->speakersSubtabs(), &QTabWidget::currentChanged, this,
                [this](int) {
            if (!m_inspectorPane || !m_inspectorPane->tabs()) {
                return;
            }
            const QString inspectorTabName =
                m_inspectorPane->tabs()->tabText(m_inspectorPane->tabs()->currentIndex());
            if (inspectorTabName.compare(QStringLiteral("Speakers"), Qt::CaseInsensitive) == 0 &&
                m_speakersTab) {
                const QString speakersSubtabName =
                    m_inspectorPane->speakersSubtabs()->tabText(
                        m_inspectorPane->speakersSubtabs()->currentIndex());
                m_speakersTab->refreshForSubtab(speakersSubtabName);
            }
        });
    }
}

void EditorWindow::setupInspectorRefreshRouting()
{
    connect(m_inspectorPane, &InspectorPane::refreshCurrentTabRequested, this, [this]() {
        QElapsedTimer refreshTimer;
        refreshTimer.start();
        refreshCurrentInspectorTab();

        const qint64 elapsedMs = refreshTimer.elapsed();
        m_lastInspectorRefreshDurationMs.store(elapsedMs);
        qint64 maxDuration = m_maxInspectorRefreshDurationMs.load();
        while (elapsedMs > maxDuration &&
               !m_maxInspectorRefreshDurationMs.compare_exchange_weak(maxDuration, elapsedMs)) {
        }
        constexpr qint64 kSlowInspectorRefreshThresholdMs = 30;
        if (elapsedMs >= kSlowInspectorRefreshThresholdMs) {
            m_inspectorRefreshSlowCount.fetch_add(1);
            if (debugPlaybackWarnEnabled()) {
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] slow inspector refresh: %1 ms")
                           .arg(elapsedMs);
            }
        } else if (debugPlaybackVerboseEnabled()) {
            playbackTrace(QStringLiteral("EditorWindow::setupInspectorRefreshRouting.refresh"),
                          QStringLiteral("elapsed_ms=%1").arg(elapsedMs));
        }
    });
    connect(m_inspectorPane, &InspectorPane::refreshTabRequested, this, [this](const QString& tabName) {
        QElapsedTimer refreshTimer;
        refreshTimer.start();
        refreshInspectorTabByName(tabName);

        const qint64 elapsedMs = refreshTimer.elapsed();
        m_lastInspectorRefreshDurationMs.store(elapsedMs);
        qint64 maxDuration = m_maxInspectorRefreshDurationMs.load();
        while (elapsedMs > maxDuration &&
               !m_maxInspectorRefreshDurationMs.compare_exchange_weak(maxDuration, elapsedMs)) {
        }
        constexpr qint64 kSlowInspectorRefreshThresholdMs = 30;
        if (elapsedMs >= kSlowInspectorRefreshThresholdMs) {
            m_inspectorRefreshSlowCount.fetch_add(1);
            if (debugPlaybackWarnEnabled()) {
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] slow inspector refresh: %1 ms")
                           .arg(elapsedMs);
            }
        } else if (debugPlaybackVerboseEnabled()) {
            playbackTrace(QStringLiteral("EditorWindow::setupInspectorRefreshRouting.refresh"),
                          QStringLiteral("elapsed_ms=%1").arg(elapsedMs));
        }
    });
}
