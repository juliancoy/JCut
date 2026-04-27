#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_flow_debug.h"

#include "decoder_context.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {
struct BoxstreamSmoothingSettings {
    bool smoothTranslation = false;
    bool smoothScale = false;
};
BoxstreamSmoothingSettings g_boxstreamSmoothingSettings;
} // namespace

bool SpeakersTab::runAutoTrackForSpeaker(const QString& speakerId, bool forceModelTracking)
{
    if (!activeCutMutable() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    }
    const QString requestedMode = forceModelTracking
        ? QStringLiteral("AutoTrack")
        : tracking.value(QString(kTranscriptSpeakerTrackingModeKey))
              .toString(QStringLiteral("AutoTrack"))
              .trimmed();
    struct AutoTrackPreflightSettings {
        bool useSpeechWindows = false;
    };
    static AutoTrackPreflightSettings s_autoTrackPreflight;

    bool hasRef1 = false;
    bool hasRef2 = false;
    const QJsonObject ref1 =
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    const QJsonObject ref2 =
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    if (!hasRef1 && !hasRef2) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Generate BoxStream"),
            QStringLiteral("Set at least one reference point (Ref 1 or Ref 2) before running auto-frame."));
        return false;
    }
    if (requestedMode.compare(QStringLiteral("AutoTrack"), Qt::CaseInsensitive) == 0) {
        QDialog preflightDialog;
        preflightDialog.setWindowTitle(QStringLiteral("AutoTrack Preflight"));
        preflightDialog.setWindowFlag(Qt::Window, true);
        preflightDialog.resize(520, 220);
        auto* layout = new QVBoxLayout(&preflightDialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);
        auto* infoLabel = new QLabel(
            QStringLiteral("Configure this AutoTrack run.\n\n"
                           "Speech windows limit tracking to when the speaker is speaking, expanded by ±2.0s."),
            &preflightDialog);
        infoLabel->setWordWrap(true);
        layout->addWidget(infoLabel);
        auto* windowsCheck =
            new QCheckBox(QStringLiteral("Use speaker speech windows (±2s)"), &preflightDialog);
        windowsCheck->setChecked(s_autoTrackPreflight.useSpeechWindows);
        layout->addWidget(windowsCheck);
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
            return false;
        }
        s_autoTrackPreflight.useSpeechWindows = windowsCheck->isChecked();
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Generate BoxStream"),
            QStringLiteral("Select a clip first."));
        return false;
    }
    const int64_t clipSourceStart = selectedClip ? qMax<int64_t>(0, selectedClip->sourceInFrame) : -1;
    const int64_t clipSourceEnd = selectedClip
        ? qMax<int64_t>(clipSourceStart, selectedClip->sourceInFrame + qMax<int64_t>(0, selectedClip->durationFrames - 1))
        : -1;

    int64_t speakerRangeStart = -1;
    int64_t speakerRangeEnd = -1;
    QVector<QPair<int64_t, int64_t>> speakerWindows;
    const bool useSpeechWindows = s_autoTrackPreflight.useSpeechWindows;
    const int64_t speechPadFrames = static_cast<int64_t>(std::round(2.0 * static_cast<double>(kTimelineFps)));
    {
        const QString targetSpeaker = speakerId.trimmed();
        const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
        for (const QJsonValue& segValue : segments) {
            const QJsonObject segmentObj = segValue.toObject();
            const QString segmentSpeaker =
                segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
            const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    continue;
                }
                QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (wordSpeaker.isEmpty()) {
                    wordSpeaker = segmentSpeaker;
                }
                if (wordSpeaker != targetSpeaker) {
                    continue;
                }
                if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                    continue;
                }
                const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                if (startSeconds < 0.0 || endSeconds < startSeconds) {
                    continue;
                }
                const int64_t startFrame = qMax<int64_t>(
                    0, static_cast<int64_t>(std::floor(startSeconds * static_cast<double>(kTimelineFps))));
                const int64_t endFrame = qMax<int64_t>(
                    startFrame, static_cast<int64_t>(std::floor(endSeconds * static_cast<double>(kTimelineFps))));
                if (selectedClip && (endFrame < clipSourceStart || startFrame > clipSourceEnd)) {
                    continue;
                }
                if (useSpeechWindows) {
                    int64_t winStart = qMax<int64_t>(0, startFrame - speechPadFrames);
                    int64_t winEnd = qMax<int64_t>(winStart, endFrame + speechPadFrames);
                    if (selectedClip) {
                        winStart = qMax<int64_t>(clipSourceStart, winStart);
                        winEnd = qMin<int64_t>(clipSourceEnd, winEnd);
                    }
                    if (winEnd >= winStart) {
                        speakerWindows.push_back(qMakePair(winStart, winEnd));
                    }
                }
                if (speakerRangeStart < 0 || startFrame < speakerRangeStart) {
                    speakerRangeStart = startFrame;
                }
                if (speakerRangeEnd < 0 || endFrame > speakerRangeEnd) {
                    speakerRangeEnd = endFrame;
                }
            }
        }
    }
    if (useSpeechWindows && !speakerWindows.isEmpty()) {
        std::sort(
            speakerWindows.begin(),
            speakerWindows.end(),
            [](const QPair<int64_t, int64_t>& a, const QPair<int64_t, int64_t>& b) {
                if (a.first == b.first) {
                    return a.second < b.second;
                }
                return a.first < b.first;
            });
        QVector<QPair<int64_t, int64_t>> merged;
        for (const auto& window : speakerWindows) {
            if (merged.isEmpty()) {
                merged.push_back(window);
                continue;
            }
            auto& last = merged.last();
            if (window.first <= (last.second + 1)) {
                last.second = qMax(last.second, window.second);
            } else {
                merged.push_back(window);
            }
        }
        speakerWindows = merged;
    }

    const auto createPoint = [](int64_t frame,
                                double x,
                                double y,
                                double confidence,
                                const QString& source,
                                double boxSize = -1.0) {
        QJsonObject p;
        const double nx = qBound(0.0, x, 1.0);
        const double ny = qBound(0.0, y, 1.0);
        p[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        p[QString(kTranscriptSpeakerLocationXKey)] = nx;
        p[QString(kTranscriptSpeakerLocationYKey)] = ny;
        if (boxSize > 0.0) {
            const double bs = qBound(0.01, boxSize, 1.0);
            p[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = bs;
            writeNormalizedFaceBox(p, nx, ny, bs);
        }
        p[QString(kTranscriptSpeakerTrackingConfidenceKey)] = qBound(0.0, confidence, 1.0);
        p[QString(kTranscriptSpeakerTrackingSourceKey)] = source;
        return p;
    };

    QJsonArray keyframes;
    bool usedNativeModel = false;
    bool usedDockerModel = false;
    QString modelError;
    const int64_t refStartFrame = hasRef1 && hasRef2
        ? qMin(ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong(),
               ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong())
        : (hasRef1
               ? ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong()
               : ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const int64_t refEndFrame = hasRef1 && hasRef2
        ? qMax(ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong(),
               ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong())
        : refStartFrame;
    int64_t trackStartFrame = qMax<int64_t>(0, refStartFrame);
    int64_t trackEndFrame = qMax<int64_t>(trackStartFrame, refEndFrame);
    if (!speakerWindows.isEmpty()) {
        trackStartFrame = qMin(trackStartFrame, speakerWindows.constFirst().first);
        trackEndFrame = qMax(trackEndFrame, speakerWindows.constLast().second);
    } else if (speakerRangeStart >= 0 && speakerRangeEnd >= 0) {
        trackStartFrame = qMax<int64_t>(0, qMin(trackStartFrame, speakerRangeStart));
        trackEndFrame = qMax<int64_t>(trackEndFrame, speakerRangeEnd);
    }
    if (selectedClip) {
        trackStartFrame = qMax<int64_t>(clipSourceStart, trackStartFrame);
        trackEndFrame = qMin<int64_t>(clipSourceEnd, trackEndFrame);
        if (trackEndFrame < trackStartFrame) {
            trackStartFrame = qMax<int64_t>(clipSourceStart, qMin<int64_t>(clipSourceEnd, refStartFrame));
            trackEndFrame = qMax<int64_t>(trackStartFrame, qMin<int64_t>(clipSourceEnd, refEndFrame));
        }
    }
    const int64_t trackSpan = qMax<int64_t>(1, trackEndFrame - trackStartFrame);
    const QJsonObject trackingConfig = transcriptSpeakerTrackingConfigSnapshot();
    const int configuredMaxStepFrames =
        qBound<int>(1, trackingConfig.value(QStringLiteral("auto_track_step_frames")).toInt(6), 120);
    const int stepFrames =
        qBound<int>(1, static_cast<int>(trackSpan / 300), configuredMaxStepFrames);

    auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
        m_loadedTranscriptPath,
        selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id,
        QFileInfo(interactivePreviewMediaPathForClip(*selectedClip)).completeBaseName());

    QString requestPath;
    QString keyframesPath;
    QString statsPath;
    QString logPath;
    QString indexPath;
    QString overwriteDecisionPath;
    auto resolveStage6Paths = [&]() {
        requestPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_boxstream_request.json").arg(debugRun.videoStem));
        keyframesPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_boxstream_output_keyframes.json").arg(debugRun.videoStem));
        statsPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_boxstream_stats.json").arg(debugRun.videoStem));
        logPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_boxstream_log.txt").arg(debugRun.videoStem));
        indexPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_index.json").arg(debugRun.videoStem));
        overwriteDecisionPath = QDir(debugRun.runDir).absoluteFilePath(
            QStringLiteral("%1_overwrite_decision.json").arg(debugRun.videoStem));
    };
    resolveStage6Paths();
    while (true) {
        QStringList existingFiles;
        const auto action = speaker_flow_debug::promptOverwrite(
            nullptr,
            debugRun.runDir,
            QStringLiteral("stage_6_boxstream"),
            QStringList{requestPath, keyframesPath, statsPath, logPath},
            true,
            &existingFiles);
        if (action == speaker_flow_debug::OverwriteAction::Cancel) {
            if (!existingFiles.isEmpty()) {
                speaker_flow_debug::recordOverwriteDecision(
                    overwriteDecisionPath,
                    debugRun.runDir,
                    QStringLiteral("stage_6_boxstream"),
                    existingFiles,
                    false);
            }
            speaker_flow_debug::persistIndex(
                indexPath,
                debugRun.runId,
                debugRun.clipToken,
                QFileInfo(selectedClip->filePath).fileName(),
                m_loadedTranscriptPath,
                QStringLiteral("stage_6_boxstream"),
                QStringLiteral("skipped"),
                QStringLiteral("Canceled due to overwrite prompt."),
                {});
            return false;
        }
        if (action == speaker_flow_debug::OverwriteAction::CreateNewRun) {
            debugRun = speaker_flow_debug::createNewRunFrom(debugRun);
            resolveStage6Paths();
            continue;
        }
        if (!existingFiles.isEmpty()) {
            speaker_flow_debug::recordOverwriteDecision(
                overwriteDecisionPath,
                debugRun.runDir,
                QStringLiteral("stage_6_boxstream"),
                existingFiles,
                true);
            for (const QString& file : existingFiles) {
                QFile::remove(file);
            }
        }
        break;
    }
    {
        QJsonObject request;
        request[QStringLiteral("run_id")] = debugRun.runId;
        request[QStringLiteral("speaker_id")] = speakerId;
        request[QStringLiteral("requested_mode")] = requestedMode;
        request[QStringLiteral("force_model_tracking")] = forceModelTracking;
        request[QStringLiteral("has_ref1")] = hasRef1;
        request[QStringLiteral("has_ref2")] = hasRef2;
        request[QStringLiteral("track_start_frame")] = static_cast<qint64>(trackStartFrame);
        request[QStringLiteral("track_end_frame")] = static_cast<qint64>(trackEndFrame);
        request[QStringLiteral("step_frames")] = stepFrames;
        request[QStringLiteral("use_speech_windows")] = useSpeechWindows;
        request[QStringLiteral("windows_count")] = speakerWindows.size();
        request[QStringLiteral("source_clip_id")] = selectedClip->id;
        request[QStringLiteral("source_file")] = selectedClip->filePath;
        QFile f(requestPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(request).toJson(QJsonDocument::Indented));
            f.close();
        }
    }

    if (requestedMode.compare(QStringLiteral("AutoTrack"), Qt::CaseInsensitive) == 0 && hasRef1 && hasRef2) {
        if (selectedClip) {
            usedNativeModel = runNativeAutoTrackForSpeaker(
                *selectedClip, speakerId, ref1, ref2, speakerWindows, trackStartFrame, trackEndFrame, stepFrames, &keyframes, &modelError);
            if (!usedNativeModel) {
                const QString allowDocker =
                    qEnvironmentVariable("SPEAKER_AUTOTRACK_ALLOW_DOCKER", QStringLiteral("0")).trimmed().toLower();
                const bool allowDockerFallback =
                    (allowDocker == QStringLiteral("1") ||
                     allowDocker == QStringLiteral("true") ||
                     allowDocker == QStringLiteral("yes") ||
                     allowDocker == QStringLiteral("on"));
                if (allowDockerFallback) {
                    QString dockerError;
                    usedDockerModel = runDockerAutoTrackForSpeaker(
                        *selectedClip, speakerId, ref1, ref2, trackStartFrame, trackEndFrame, stepFrames, &keyframes, &dockerError);
                    if (!dockerError.isEmpty()) {
                        if (modelError.isEmpty()) {
                            modelError = dockerError;
                        } else {
                            modelError += QStringLiteral("\n") + dockerError;
                        }
                    }
                }
            }
        } else {
            modelError = QStringLiteral("no selected clip for native autotrack");
        }
    }

    if (!usedNativeModel && !usedDockerModel) {
        if (modelError.trimmed().startsWith(QStringLiteral("user canceled"), Qt::CaseInsensitive)) {
            QFile log(logPath);
            if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                log.write(modelError.toUtf8());
                log.close();
            }
            speaker_flow_debug::persistIndex(
                indexPath,
                debugRun.runId,
                debugRun.clipToken,
                QFileInfo(selectedClip->filePath).fileName(),
                m_loadedTranscriptPath,
                QStringLiteral("stage_6_boxstream"),
                QStringLiteral("skipped"),
                QStringLiteral("User canceled model tracking."),
                {requestPath, logPath});
            return false;
        }
        if (hasRef1 && hasRef2) {
            const int64_t frame1 = ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const int64_t frame2 = ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const double x1 = qBound(0.0, ref1.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const double y1 = qBound(0.0, ref1.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
            const double x2 = qBound(0.0, ref2.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const double y2 = qBound(0.0, ref2.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
            const double box1 = qBound(
                -1.0, ref1.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
            const double box2 = qBound(
                -1.0, ref2.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
            const bool hasBox1 = box1 > 0.0;
            const bool hasBox2 = box2 > 0.0;

            QVector<QPair<int64_t, int64_t>> linearWindows = speakerWindows;
            if (linearWindows.isEmpty()) {
                linearWindows.push_back(qMakePair(trackStartFrame, trackEndFrame));
            }
            const int64_t kStepFrames = qMax<int64_t>(1, stepFrames);
            for (const auto& window : linearWindows) {
                const int64_t startFrame = qMin(window.first, window.second);
                const int64_t endFrame = qMax(window.first, window.second);
                const int64_t span = qMax<int64_t>(1, endFrame - startFrame);
                for (int64_t frame = startFrame; frame <= endFrame; frame += kStepFrames) {
                    const double t = static_cast<double>(frame - startFrame) / static_cast<double>(span);
                    const double x = x1 + (x2 - x1) * t;
                    const double y = y1 + (y2 - y1) * t;
                    double boxSize = -1.0;
                    if (hasBox1 && hasBox2) {
                        boxSize = box1 + (box2 - box1) * t;
                    } else if (hasBox1) {
                        boxSize = box1;
                    } else if (hasBox2) {
                        boxSize = box2;
                    }
                    keyframes.push_back(createPoint(
                        frame, x, y, 0.70, QStringLiteral("autotrack_linear_v1"), boxSize));
                }
                const int64_t lastFrame = keyframes.isEmpty()
                    ? -1
                    : keyframes.at(keyframes.size() - 1)
                          .toObject()
                          .value(QString(kTranscriptSpeakerTrackingFrameKey))
                          .toVariant()
                          .toLongLong();
                if (keyframes.isEmpty() || lastFrame != endFrame) {
                    double boxSize = -1.0;
                    if (hasBox2) {
                        boxSize = box2;
                    } else if (hasBox1) {
                        boxSize = box1;
                    }
                    keyframes.push_back(createPoint(
                        endFrame, x2, y2, 0.70, QStringLiteral("autotrack_linear_v1"), boxSize));
                }
            }
            tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AutoTrackLinear");
            tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] =
                QStringLiteral("completed_linear_v1_open_for_model_tracking");
        } else {
            const QJsonObject onlyRef = hasRef1 ? ref1 : ref2;
            const int64_t frame = onlyRef.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const double x = qBound(0.0, onlyRef.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const double y = qBound(0.0, onlyRef.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
            const double boxSize =
                qBound(-1.0, onlyRef.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
            keyframes.push_back(createPoint(
                frame, x, y, 0.60, QStringLiteral("autotrack_anchor_v1"), boxSize));
            tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AnchorHold");
            tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] =
                QStringLiteral("completed_anchor_v1_open_for_model_tracking");
        }
        if (!modelError.isEmpty()) {
            QMessageBox::information(
                nullptr,
                QStringLiteral("AutoTrack Fallback"),
                QStringLiteral("Model AutoTrack was unavailable, so linear fallback was used.\n\n%1")
                    .arg(modelError));
        }
    } else {
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AutoTrack");
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = usedNativeModel
            ? QStringLiteral("completed_native_cpp_v1")
            : QStringLiteral("completed_docker_v1");
    }

    if (selectedClip && m_speakerDeps.updateClipById) {
        m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
            editableClip.speakerFramingSpeakerId = speakerId;
            // Generate BoxStream only writes tracking data.
            // Runtime Face Stabilize now evaluates transforms on the fly.
            editableClip.speakerFramingKeyframes.clear();
            normalizeClipTransformKeyframes(editableClip);
        });
    }

    tracking[QString(kTranscriptSpeakerTrackingKeyframesKey)] = keyframes;
    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    const bool saved = engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);

    QFile kfFile(keyframesPath);
    if (kfFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        kfFile.write(QJsonDocument(keyframes).toJson(QJsonDocument::Indented));
        kfFile.close();
    }
    QJsonObject stats;
    stats[QStringLiteral("run_id")] = debugRun.runId;
    stats[QStringLiteral("speaker_id")] = speakerId;
    stats[QStringLiteral("used_native_model")] = usedNativeModel;
    stats[QStringLiteral("used_docker_model")] = usedDockerModel;
    stats[QStringLiteral("fallback_used")] = (!usedNativeModel && !usedDockerModel);
    stats[QStringLiteral("keyframe_count")] = keyframes.size();
    stats[QStringLiteral("track_start_frame")] = static_cast<qint64>(trackStartFrame);
    stats[QStringLiteral("track_end_frame")] = static_cast<qint64>(trackEndFrame);
    stats[QStringLiteral("step_frames")] = stepFrames;
    stats[QStringLiteral("mode_written")] = tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString();
    stats[QStringLiteral("auto_state")] = tracking.value(QString(kTranscriptSpeakerTrackingAutoStateKey)).toString();
    stats[QStringLiteral("transcript_saved")] = saved;
    if (!modelError.isEmpty()) {
        stats[QStringLiteral("model_error")] = modelError;
    }
    QFile statsFile(statsPath);
    if (statsFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        statsFile.write(QJsonDocument(stats).toJson(QJsonDocument::Indented));
        statsFile.close();
    }
    QFile log(logPath);
    if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QStringList logLines;
        logLines << QStringLiteral("run_id=%1").arg(debugRun.runId)
                 << QStringLiteral("speaker_id=%1").arg(speakerId)
                 << QStringLiteral("used_native_model=%1").arg(usedNativeModel ? QStringLiteral("true") : QStringLiteral("false"))
                 << QStringLiteral("used_docker_model=%1").arg(usedDockerModel ? QStringLiteral("true") : QStringLiteral("false"))
                 << QStringLiteral("fallback_used=%1").arg((!usedNativeModel && !usedDockerModel) ? QStringLiteral("true") : QStringLiteral("false"))
                 << QStringLiteral("keyframe_count=%1").arg(keyframes.size())
                 << QStringLiteral("transcript_saved=%1").arg(saved ? QStringLiteral("true") : QStringLiteral("false"));
        if (!modelError.isEmpty()) {
            logLines << QStringLiteral("model_error=%1").arg(modelError);
        }
        log.write(logLines.join(QLatin1Char('\n')).toUtf8());
        log.close();
    }
    speaker_flow_debug::persistIndex(
        indexPath,
        debugRun.runId,
        debugRun.clipToken,
        QFileInfo(selectedClip->filePath).fileName(),
        m_loadedTranscriptPath,
        QStringLiteral("stage_6_boxstream"),
        saved ? QStringLiteral("ok") : QStringLiteral("error"),
        saved ? QStringLiteral("BoxStream completed.") : QStringLiteral("Failed to save transcript after BoxStream."),
        {requestPath, keyframesPath, statsPath, logPath});
    return saved;
}

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!runAutoTrackForSpeaker(speakerId, true)) {
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

void SpeakersTab::onSpeakerBoxStreamSettingsClicked()
{
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("BoxStream Settings"));
    dialog.setWindowFlag(Qt::Window, true);
    dialog.resize(520, 230);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* infoLabel = new QLabel(
        QStringLiteral("Configure BoxStream-related smoothing options.\n\n"
                       "These are not part of AutoTrack preflight so run setup stays minimal.\n"
                       "Generate BoxStream itself does not apply clip transforms."),
        &dialog);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto* smoothTranslationCheck =
        new QCheckBox(QStringLiteral("Smooth translation keyframes (post-solve)"), &dialog);
    smoothTranslationCheck->setChecked(g_boxstreamSmoothingSettings.smoothTranslation);
    layout->addWidget(smoothTranslationCheck);

    auto* smoothScaleCheck =
        new QCheckBox(QStringLiteral("Smooth scale keyframes (post-solve)"), &dialog);
    smoothScaleCheck->setChecked(g_boxstreamSmoothingSettings.smoothScale);
    layout->addWidget(smoothScaleCheck);

    auto* noteLabel = new QLabel(
        QStringLiteral("Current default is OFF for all smoothing."),
        &dialog);
    noteLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    layout->addWidget(noteLabel);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* saveButton = new QPushButton(QStringLiteral("Save"), &dialog);
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    layout->addLayout(buttons);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    g_boxstreamSmoothingSettings.smoothTranslation = smoothTranslationCheck->isChecked();
    g_boxstreamSmoothingSettings.smoothScale = smoothScaleCheck->isChecked();
}

void SpeakersTab::onSpeakerGuideClicked()
{
    const QString guideText =
        QStringLiteral(
            "Subtitle Face Tracking Guide\n\n"
            "1. Select a speaker row. This arms Ref 1 automatically.\n"
            "2. Move the playhead to a frame where the speaker is visible.\n"
            "3. In Preview, hold Shift.\n"
            "4. Drag a square over the speaker's head and release.\n"
            "5. Click Generate BoxStream immediately (single-reference mode).\n"
            "6. Optional: set Ref 2 on another frame, then Generate BoxStream again for better quality.\n\n"
            "FaceBox Target\n"
            "- In Speakers, set Face X / Face Y for desired on-screen face position.\n"
            "- Toggle Show FaceBox to show/hide the yellow target box in Preview.\n"
            "- The yellow box in Preview is the target box faces are fit into.\n\n"
            "Face Stabilize\n"
            "- Face Stabilize is a separate clip-level toggle.\n"
            "- It applies generated face keyframes to the selected clip.\n\n"
            "Tips\n"
            "- Square selection is required and enforced.\n"
            "- Ref buttons show [ARMED] when awaiting your pick.\n"
            "- Speaker metadata is editable only on derived cuts (not Original).");
    QMessageBox::information(nullptr, QStringLiteral("Subtitle Face Tracking Guide"), guideText);
}

void SpeakersTab::onSpeakerFramingTargetChanged()
{
    if (!activeCutMutable()) {
        updateSpeakerFramingTargetControls();
        if (m_speakerDeps.refreshPreview) {
            m_speakerDeps.refreshPreview();
        }
        return;
    }
    if (!saveClipSpeakerFramingTargetsFromControls()) {
        if (m_speakerDeps.refreshPreview) {
            m_speakerDeps.refreshPreview();
        }
        return;
    }
    if (m_speakerDeps.refreshPreview) {
        m_speakerDeps.refreshPreview();
    }
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakerFramingZoomEnabledChanged(bool checked)
{
    if (m_widgets.speakerFramingTargetBoxSpin) {
        m_widgets.speakerFramingTargetBoxSpin->setEnabled(
            checked && activeCutMutable() && !selectedSpeakerId().isEmpty());
    }
    onSpeakerFramingTargetChanged();
    if (m_speakerDeps.refreshPreview) {
        m_speakerDeps.refreshPreview();
    }
}

void SpeakersTab::onSpeakerApplyFramingToClipChanged(bool checked)
{
    Q_UNUSED(checked)
    if (!activeCutMutable()) {
        updateSpeakerFramingTargetControls();
        return;
    }
    if (!saveClipSpeakerFramingEnabledFromControls()) {
        updateSpeakerFramingTargetControls();
        return;
    }
    updateSpeakerTrackingStatusLabel();
}

bool SpeakersTab::handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm)
{
    if (m_pendingReferencePick <= 0 || m_pendingReferencePick > 2 || !activeCutMutable()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->id != clipId) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        m_pendingReferencePick = 0;
        updateSpeakerTrackingStatusLabel();
        return false;
    }
    const int64_t frame = currentSourceFrameForClip(*clip);
    if (!saveSpeakerTrackingReferenceAt(speakerId, m_pendingReferencePick, frame, xNorm, yNorm)) {
        m_pendingReferencePick = 0;
        refresh();
        return false;
    }

    m_pendingReferencePick = 0;
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    return true;
}

bool SpeakersTab::handlePreviewBox(const QString& clipId,
                                   qreal xNorm,
                                   qreal yNorm,
                                   qreal boxSizeNorm)
{
    if (m_pendingReferencePick <= 0 || m_pendingReferencePick > 2 || !activeCutMutable()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->id != clipId) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        m_pendingReferencePick = 0;
        updateSpeakerTrackingStatusLabel();
        return false;
    }
    const int64_t frame = currentSourceFrameForClip(*clip);
    if (!saveSpeakerTrackingReferenceAt(
            speakerId,
            m_pendingReferencePick,
            frame,
            xNorm,
            yNorm,
            qBound<qreal>(0.01, boxSizeNorm, 1.0))) {
        m_pendingReferencePick = 0;
        refresh();
        return false;
    }

    m_pendingReferencePick = 0;
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    return true;
}
