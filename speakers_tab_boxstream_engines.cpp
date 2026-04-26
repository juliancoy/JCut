#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "decoder_context.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
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
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

bool SpeakersTab::cycleFramingModeForSpeaker(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    bool hasRef1 = false;
    bool hasRef2 = false;
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    const QString currentMode =
        tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual")).trimmed();

    QString nextMode = QStringLiteral("Manual");
    if (currentMode.compare(QStringLiteral("manual"), Qt::CaseInsensitive) == 0) {
        nextMode = QStringLiteral("ReferencePoints");
    } else if (currentMode.compare(QStringLiteral("referencepoints"), Qt::CaseInsensitive) == 0) {
        nextMode = hasPointstream ? QStringLiteral("AutoTrack") : QStringLiteral("Manual");
    } else {
        nextMode = QStringLiteral("Manual");
    }

    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = nextMode;
    if (nextMode == QStringLiteral("Manual")) {
        tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("manual");
    } else if (nextMode == QStringLiteral("ReferencePoints")) {
        tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("refs_only");
    }

    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::runNativeAutoTrackForSpeaker(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& ref1,
                                               const QJsonObject& ref2,
                                               const QVector<QPair<int64_t, int64_t>>& activeWindows,
                                               int64_t startFrame,
                                               int64_t endFrame,
                                               int stepFrames,
                                               QJsonArray* keyframesOut,
                                               QString* errorOut)
{
    Q_UNUSED(speakerId);
    if (!keyframesOut) {
        return false;
    }
    *keyframesOut = QJsonArray();

    auto resolveMediaPath = [&](const TimelineClip& currentClip, QString* reasonOut) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
        QFileInfo candidateInfo(candidate);
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
            if (reasonOut) {
                *reasonOut = QStringLiteral("interactive media path invalid (%1); using source (%2)")
                                 .arg(candidate, sourcePath);
            }
            return sourcePath;
        }
        if (reasonOut) {
            *reasonOut = QStringLiteral("no playable media for native autotrack");
        }
        return QString();
    };

    QString mediaReason;
    const QString mediaPath = resolveMediaPath(clip, &mediaReason);
    if (mediaPath.isEmpty()) {
        if (errorOut) {
            *errorOut = mediaReason.isEmpty() ? QStringLiteral("native autotrack media path not found") : mediaReason;
        }
        return false;
    }

    editor::DecoderContext decoder(mediaPath);
    if (!decoder.initialize()) {
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack failed to initialize decoder: %1").arg(mediaPath);
        }
        return false;
    }

    const auto frameFromRef = [](const QJsonObject& refObj) -> int64_t {
        return refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
    };
    const auto xFromRef = [](const QJsonObject& refObj) -> qreal {
        return qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    };
    const auto yFromRef = [](const QJsonObject& refObj) -> qreal {
        return qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    };
    const auto boxFromRef = [](const QJsonObject& refObj) -> qreal {
        return qBound<qreal>(0.01, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.12), 1.0);
    };

    const double sourceFps = clip.sourceFps > 0.0
        ? clip.sourceFps
        : (decoder.info().fps > 0.0 ? decoder.info().fps : 30.0);
    const int64_t ref1Frame = frameFromRef(ref1);
    const int64_t ref2Frame = frameFromRef(ref2);
    const int64_t refStart = qMin(ref1Frame, ref2Frame);
    const int64_t refEnd = qMax(ref1Frame, ref2Frame);

    const int64_t ref1SourceFrame = canonicalToSourceFrameForTracking(ref1Frame, sourceFps);
    const int64_t ref2SourceFrame = canonicalToSourceFrameForTracking(ref2Frame, sourceFps);
    const QImage ref1Image =
        toGray8(decoder.decodeFrame(ref1SourceFrame).cpuImage());
    const QImage ref2Image =
        toGray8(decoder.decodeFrame(ref2SourceFrame).cpuImage());
    if (ref1Image.isNull() || ref2Image.isNull()) {
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack failed to decode reference frame(s)");
        }
        return false;
    }

    QImage tmpl1;
    QImage tmpl2;
    if (!cropSquareGray(ref1Image, xFromRef(ref1), yFromRef(ref1), boxFromRef(ref1), &tmpl1, nullptr, nullptr, nullptr) ||
        !cropSquareGray(ref2Image, xFromRef(ref2), yFromRef(ref2), boxFromRef(ref2), &tmpl2, nullptr, nullptr, nullptr)) {
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack failed to create templates");
        }
        return false;
    }

    const int64_t start = qMin(startFrame, endFrame);
    const int64_t end = qMax(startFrame, endFrame);
    const int64_t spanRef = qMax<int64_t>(1, refEnd - refStart);
    const int64_t step = qMax<int64_t>(1, stepFrames);
    QVector<QPair<int64_t, int64_t>> windows = activeWindows;
    if (windows.isEmpty()) {
        windows.push_back(qMakePair(start, end));
    }
    std::sort(
        windows.begin(),
        windows.end(),
        [](const QPair<int64_t, int64_t>& a, const QPair<int64_t, int64_t>& b) {
            if (a.first == b.first) {
                return a.second < b.second;
            }
            return a.first < b.first;
        });
    QVector<QPair<int64_t, int64_t>> mergedWindows;
    for (const auto& window : windows) {
        const int64_t wStart = qMin(window.first, window.second);
        const int64_t wEnd = qMax(window.first, window.second);
        if (mergedWindows.isEmpty()) {
            mergedWindows.push_back(qMakePair(wStart, wEnd));
            continue;
        }
        auto& last = mergedWindows.last();
        if (wStart <= (last.second + step)) {
            last.second = qMax(last.second, wEnd);
        } else {
            mergedWindows.push_back(qMakePair(wStart, wEnd));
        }
    }
    int totalSteps = 0;
    for (const auto& window : mergedWindows) {
        totalSteps += qMax(1, static_cast<int>(((qMax<int64_t>(0, window.second - window.first)) / step) + 1));
    }
    totalSteps = qMax(1, totalSteps);
    const qreal minTrackConfidence = 0.24;
    const qreal minAcceptConfidence = 0.15;
    const int reacquireInterval = 12;
    const int hardLostThreshold = 5;

    QDialog progressDialog;
    progressDialog.setWindowTitle(QStringLiteral("AutoTrack Progress"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.setWindowModality(Qt::NonModal);
    progressDialog.resize(560, 420);
    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(12, 12, 12, 12);
    progressLayout->setSpacing(8);
    auto* progressStatus = new QLabel(QStringLiteral("Initializing native tracker..."), &progressDialog);
    progressStatus->setWordWrap(true);
    progressLayout->addWidget(progressStatus);
    auto* progressPreview = new QLabel(&progressDialog);
    progressPreview->setMinimumSize(480, 270);
    progressPreview->setAlignment(Qt::AlignCenter);
    progressPreview->setStyleSheet(
        QStringLiteral("QLabel { background: #060a10; border: 1px solid #263344; border-radius: 6px; color: #8ea5c2; }"));
    progressPreview->setText(QStringLiteral("Preparing preview..."));
    progressLayout->addWidget(progressPreview, 1);
    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, totalSteps);
    progressBar->setValue(0);
    progressLayout->addWidget(progressBar);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    progressLayout->addWidget(cancelButton, 0, Qt::AlignRight);
    bool canceledByUser = false;
    connect(cancelButton, &QPushButton::clicked, &progressDialog, [&]() {
        canceledByUser = true;
        progressStatus->setText(QStringLiteral("Canceling..."));
    });
    progressDialog.show();
    QApplication::processEvents();

    qreal lastX = -1.0;
    qreal lastY = -1.0;
    qreal lastBox = -1.0;
    qreal velocityX = 0.0;
    qreal velocityY = 0.0;
    int lostCount = 0;
    enum class TrackingState {
        Tracking,
        Reacquire,
        Lost
    };
    TrackingState state = TrackingState::Reacquire;
    QJsonArray out;
    int64_t processed = 0;
    for (const auto& window : mergedWindows) {
        const int64_t windowStart = qMin(window.first, window.second);
        const int64_t windowEnd = qMax(window.first, window.second);
        // Reset motion state between non-contiguous speaking windows.
        lastX = -1.0;
        lastY = -1.0;
        lastBox = -1.0;
        velocityX = 0.0;
        velocityY = 0.0;
        lostCount = 0;
        state = TrackingState::Reacquire;
        for (int64_t frame = windowStart; frame <= windowEnd; frame += step) {
        qreal predictedX = 0.5;
        qreal predictedY = 0.5;
        qreal predictedBox = 0.12;
        if (lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            const qreal stepFactor = qBound<qreal>(1.0, static_cast<qreal>(step) / 3.0, 20.0);
            predictedX = lastX + (velocityX * stepFactor);
            predictedY = lastY + (velocityY * stepFactor);
            predictedBox = lastBox;
        } else {
            const qreal t = qBound<qreal>(
                0.0, static_cast<qreal>(frame - refStart) / static_cast<qreal>(spanRef), 1.0);
            predictedX = xFromRef(ref1) + ((xFromRef(ref2) - xFromRef(ref1)) * t);
            predictedY = yFromRef(ref1) + ((yFromRef(ref2) - yFromRef(ref1)) * t);
            predictedBox = boxFromRef(ref1) + ((boxFromRef(ref2) - boxFromRef(ref1)) * t);
        }

        const int64_t sourceFrame = canonicalToSourceFrameForTracking(frame, sourceFps);
        const QImage frameGray = toGray8(decoder.decodeFrame(sourceFrame).cpuImage());
        if (frameGray.isNull()) {
            continue;
        }
        if (canceledByUser) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled native autotrack");
            }
            return false;
        }

        struct Candidate {
            bool ok = false;
            qreal x = 0.0;
            qreal y = 0.0;
            qreal b = 0.12;
            qreal conf = 0.0;
            QString source;
            qreal score = -1.0;
        };
        auto evaluateCandidate = [&](const QImage& tmpl,
                                     const QString& sourceTag,
                                     qreal searchScale,
                                     int strideCap) {
            Candidate c;
            c.source = sourceTag;
            c.ok = trackTemplateSad(
                frameGray, predictedX, predictedY, predictedBox, tmpl, searchScale, strideCap,
                &c.x, &c.y, &c.b, &c.conf);
            if (!c.ok) {
                return c;
            }
            const qreal movePenalty =
                std::sqrt(std::pow(c.x - predictedX, 2.0) + std::pow(c.y - predictedY, 2.0)) * 0.35;
            c.score = c.conf - movePenalty;
            return c;
        };

        const bool periodicReacquire = (processed % reacquireInterval) == 0;
        const bool shouldReacquire = (state != TrackingState::Tracking) || periodicReacquire;
        const qreal narrowSearchScale = 1.8;
        const qreal wideSearchScale = 3.4;

        Candidate best;
        {
            const Candidate n1 = evaluateCandidate(
                tmpl1, QStringLiteral("autotrack_native_cpp_v2_template1"), narrowSearchScale, 3);
            const Candidate n2 = evaluateCandidate(
                tmpl2, QStringLiteral("autotrack_native_cpp_v2_template2"), narrowSearchScale, 3);
            if (n1.ok && (!best.ok || n1.score > best.score)) {
                best = n1;
            }
            if (n2.ok && (!best.ok || n2.score > best.score)) {
                best = n2;
            }
        }
        if (shouldReacquire && (!best.ok || best.conf < minTrackConfidence)) {
            const Candidate w1 = evaluateCandidate(
                tmpl1, QStringLiteral("autotrack_native_cpp_v2_reacquire1"), wideSearchScale, 5);
            const Candidate w2 = evaluateCandidate(
                tmpl2, QStringLiteral("autotrack_native_cpp_v2_reacquire2"), wideSearchScale, 5);
            if (w1.ok && (!best.ok || w1.score > best.score)) {
                best = w1;
            }
            if (w2.ok && (!best.ok || w2.score > best.score)) {
                best = w2;
            }
        }
        if (!best.ok) {
            continue;
        }

        qreal x = best.x;
        qreal y = best.y;
        qreal b = best.b;
        qreal c = best.conf;
        QString source = best.source;

        if (lastX >= 0.0 && lastY >= 0.0) {
            const qreal jumpDistance = std::sqrt(std::pow(x - lastX, 2.0) + std::pow(y - lastY, 2.0));
            const qreal maxJump = 0.10 + (0.02 * static_cast<qreal>(qMin(lostCount, 8)));
            if (jumpDistance > maxJump && c < 0.45) {
                x = (0.80 * lastX) + (0.20 * x);
                y = (0.80 * lastY) + (0.20 * y);
                b = (0.80 * lastBox) + (0.20 * b);
                source = QStringLiteral("autotrack_native_cpp_v2_jump_clamped");
            }
        }

        if (c < minAcceptConfidence && lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            x = lastX;
            y = lastY;
            b = lastBox;
            ++lostCount;
            state = (lostCount >= hardLostThreshold) ? TrackingState::Lost : TrackingState::Reacquire;
            c = (lostCount >= hardLostThreshold) ? 0.02 : 0.08;
            source = (lostCount >= hardLostThreshold)
                ? QStringLiteral("autotrack_native_cpp_v2_offscreen_hold")
                : QStringLiteral("autotrack_native_cpp_v2_hold");
        } else {
            lostCount = 0;
            state = (c >= minTrackConfidence) ? TrackingState::Tracking : TrackingState::Reacquire;
        }

        if (lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            const qreal smoothingAlpha = (c >= minTrackConfidence) ? 0.55 : 0.28;
            x = (lastX * (1.0 - smoothingAlpha)) + (x * smoothingAlpha);
            y = (lastY * (1.0 - smoothingAlpha)) + (y * smoothingAlpha);
            b = (lastBox * (1.0 - smoothingAlpha)) + (b * smoothingAlpha);
        }

        if (lastX >= 0.0 && lastY >= 0.0) {
            const qreal blend = 0.45;
            const qreal instVx = x - lastX;
            const qreal instVy = y - lastY;
            velocityX = (velocityX * (1.0 - blend)) + (instVx * blend);
            velocityY = (velocityY * (1.0 - blend)) + (instVy * blend);
        } else {
            velocityX = 0.0;
            velocityY = 0.0;
        }
        lastX = qBound<qreal>(0.0, x, 1.0);
        lastY = qBound<qreal>(0.0, y, 1.0);
        lastBox = qBound<qreal>(0.01, b, 1.0);
        QJsonObject point;
        point[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        point[QString(kTranscriptSpeakerLocationXKey)] = lastX;
        point[QString(kTranscriptSpeakerLocationYKey)] = lastY;
        point[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = lastBox;
        writeNormalizedFaceBox(point, lastX, lastY, lastBox);
        point[QString(kTranscriptSpeakerTrackingConfidenceKey)] = qBound<qreal>(0.0, c, 1.0);
        point[QString(kTranscriptSpeakerTrackingSourceKey)] = source;
        out.push_back(point);
        ++processed;

        const int stepIndex = qMax(0, qMin(totalSteps, static_cast<int>(processed)));
        progressBar->setValue(stepIndex);
        progressStatus->setText(
            QStringLiteral("Tracking frame %1/%2 | conf=%3 | mode=%4")
                .arg(stepIndex)
                .arg(totalSteps)
                .arg(QString::number(c, 'f', 3))
                .arg(source));

        QImage preview = frameGray.convertToFormat(QImage::Format_RGB32);
        {
            QPainter painter(&preview);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const int pw = preview.width();
            const int ph = preview.height();
            const int minSide = qMax(1, qMin(pw, ph));
            const int cx = static_cast<int>(std::round(lastX * static_cast<qreal>(pw)));
            const int cy = static_cast<int>(std::round(lastY * static_cast<qreal>(ph)));
            const int side = qBound(12, static_cast<int>(std::round(lastBox * static_cast<qreal>(minSide))), minSide);
            const QRect boxRect(
                qBound(0, cx - (side / 2), qMax(0, pw - side)),
                qBound(0, cy - (side / 2), qMax(0, ph - side)),
                side,
                side);
            painter.setPen(QPen(QColor(QStringLiteral("#2fcf73")), 2));
            painter.drawRect(boxRect);
            painter.setPen(QPen(QColor(QStringLiteral("#ffdf5d")), 1));
            painter.drawLine(cx - 8, cy, cx + 8, cy);
            painter.drawLine(cx, cy - 8, cx, cy + 8);
        }
        progressPreview->setPixmap(QPixmap::fromImage(
            preview.scaled(
                progressPreview->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation)));
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (canceledByUser) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled native autotrack");
            }
            return false;
        }
    }
        if (!out.isEmpty() && lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            const int64_t lastFrame = out.at(out.size() - 1).toObject().value(
                QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            if (lastFrame != windowEnd) {
                QJsonObject tail;
                tail[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(windowEnd);
                tail[QString(kTranscriptSpeakerLocationXKey)] = lastX;
                tail[QString(kTranscriptSpeakerLocationYKey)] = lastY;
                tail[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = lastBox;
                writeNormalizedFaceBox(tail, lastX, lastY, lastBox);
                tail[QString(kTranscriptSpeakerTrackingConfidenceKey)] = 0.5;
                tail[QString(kTranscriptSpeakerTrackingSourceKey)] = QStringLiteral("autotrack_native_cpp_v2_tail");
                out.push_back(tail);
            }
        }
    }

    if (out.isEmpty() || processed < 2) {
        progressDialog.close();
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack produced insufficient keyframes");
        }
        return false;
    }
    progressBar->setValue(totalSteps);
    progressStatus->setText(QStringLiteral("Done."));
    progressDialog.close();
    *keyframesOut = out;
    return true;
}

bool SpeakersTab::runDockerAutoTrackForSpeaker(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& ref1,
                                               const QJsonObject& ref2,
                                               int64_t startFrame,
                                               int64_t endFrame,
                                               int stepFrames,
                                               QJsonArray* keyframesOut,
                                               QString* errorOut)
{
    Q_UNUSED(speakerId);
    if (!keyframesOut) {
        return false;
    }
    *keyframesOut = QJsonArray();

    const QString dockerPath = QStandardPaths::findExecutable(QStringLiteral("docker"));
    if (dockerPath.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("docker not found in PATH");
        }
        return false;
    }

    const QString scriptPath =
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("speaker_boxstream.py"));
    if (!QFileInfo::exists(scriptPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("speaker_boxstream.py not found");
        }
        return false;
    }

    auto resolveDockerAutotrackMediaPath = [&](const TimelineClip& currentClip, QString* reasonOut) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
        QFileInfo candidateInfo(candidate);
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
            sourceInfo.isFile() &&
            !isImageSequencePath(sourcePath)) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("interactive media path was non-video (%1); using source file (%2)")
                                 .arg(candidate, sourcePath);
            }
            return sourcePath;
        }
        if (reasonOut) {
            *reasonOut = QStringLiteral("interactive media path is not a playable video or image sequence: %1")
                             .arg(candidate);
        }
        return QString();
    };

    QString mediaPathReason;
    const QString mediaPath = resolveDockerAutotrackMediaPath(clip, &mediaPathReason);
    const QFileInfo mediaInfo(mediaPath);
    const bool mediaIsSequenceDir =
        !mediaPath.isEmpty() &&
        mediaInfo.exists() &&
        mediaInfo.isDir() &&
        isImageSequencePath(mediaPath);
    if (mediaPath.isEmpty() ||
        !mediaInfo.exists() ||
        (!mediaInfo.isFile() && !mediaIsSequenceDir)) {
        if (errorOut) {
            if (mediaPathReason.isEmpty()) {
                *errorOut = QStringLiteral("autotrack requires a playable video or image sequence, but none was found");
            } else {
                *errorOut = mediaPathReason;
            }
        }
        return false;
    }

    QTemporaryDir outDir;
    if (!outDir.isValid()) {
        if (errorOut) {
            *errorOut = QStringLiteral("failed to create temporary output directory");
        }
        return false;
    }

    const auto refFieldDouble = [](const QJsonObject& refObj, const QLatin1String& key, double fallback) {
        return refObj.value(QString(key)).toDouble(fallback);
    };
    const auto refFieldFrame = [](const QJsonObject& refObj) {
        return refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
    };

    const QString dockerImage = qEnvironmentVariable(
        "SPEAKER_AUTOTRACK_DOCKER_IMAGE",
        QStringLiteral("speaker-autotrack:latest"));
    const QString dockerfilePath = qEnvironmentVariable(
        "SPEAKER_AUTOTRACK_DOCKERFILE",
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("speaker_boxstream.dockerfile")));
    const QString dockerBuildContext = qEnvironmentVariable(
        "SPEAKER_AUTOTRACK_DOCKER_CONTEXT",
        QDir::currentPath());
    const QString outputFile = QDir(outDir.path()).absoluteFilePath(QStringLiteral("speaker_track.json"));
    const QString preferGpuEnv =
        qEnvironmentVariable("SPEAKER_AUTOTRACK_PREFER_GPU", QStringLiteral("1")).trimmed().toLower();
    const bool preferGpuDefault =
        !(preferGpuEnv == QStringLiteral("0") ||
          preferGpuEnv == QStringLiteral("false") ||
          preferGpuEnv == QStringLiteral("no") ||
          preferGpuEnv == QStringLiteral("off"));
    struct PreflightSettings {
        bool preferGpu = true;
        bool autoBuildMissingImage = true;
        bool forceRebuildImage = false;
        bool confirmRunCommand = true;
    };
    static bool s_preflightInitialized = false;
    static PreflightSettings s_preflightDefaults;
    if (!s_preflightInitialized) {
        s_preflightDefaults.preferGpu = preferGpuDefault;
        s_preflightInitialized = true;
    }
    PreflightSettings preflight = s_preflightDefaults;

    const auto shellQuote = [](const QString& arg) {
        QString escaped = arg;
        escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + escaped + QStringLiteral("'");
    };
    const auto formatCommand = [&](const QString& program, const QStringList& args) {
        QStringList parts;
        parts.reserve(args.size() + 1);
        parts.push_back(shellQuote(program));
        for (const QString& arg : args) {
            parts.push_back(shellQuote(arg));
        }
        return parts.join(QStringLiteral(" "));
    };
    const QStringList inspectArgs = {
        QStringLiteral("image"),
        QStringLiteral("inspect"),
        dockerImage.trimmed()
    };
    const QStringList buildArgs = {
        QStringLiteral("build"),
        QStringLiteral("-f"),
        dockerfilePath,
        QStringLiteral("-t"),
        dockerImage.trimmed(),
        dockerBuildContext
    };

    auto buildRunArgs = [&](bool gpu) {
        QStringList args;
        args << QStringLiteral("run")
             << QStringLiteral("--rm");
        if (gpu) {
            args << QStringLiteral("--gpus") << QStringLiteral("all");
        }
        args << QStringLiteral("-v") << QStringLiteral("%1:/work/app:ro").arg(QFileInfo(scriptPath).absolutePath())
             << QStringLiteral("-v") << QStringLiteral("%1:/work/video:ro").arg(mediaInfo.absolutePath())
             << QStringLiteral("-v") << QStringLiteral("%1:/work/out").arg(outDir.path())
             << dockerImage.trimmed()
             << QStringLiteral("python")
             << QStringLiteral("/work/app/%1").arg(QFileInfo(scriptPath).fileName())
             << QStringLiteral("--video") << QStringLiteral("/work/video/%1").arg(mediaInfo.fileName())
             << QStringLiteral("--output") << QStringLiteral("/work/out/%1").arg(QFileInfo(outputFile).fileName())
             << QStringLiteral("--ref1-frame") << QString::number(refFieldFrame(ref1))
             << QStringLiteral("--ref1-x") << QString::number(refFieldDouble(ref1, kTranscriptSpeakerLocationXKey, 0.5), 'f', 8)
             << QStringLiteral("--ref1-y") << QString::number(refFieldDouble(ref1, kTranscriptSpeakerLocationYKey, 0.85), 'f', 8)
             << QStringLiteral("--ref1-box") << QString::number(qBound(0.01, refFieldDouble(ref1, kTranscriptSpeakerTrackingBoxSizeKey, 0.33), 1.0), 'f', 8)
             << QStringLiteral("--ref2-frame") << QString::number(refFieldFrame(ref2))
             << QStringLiteral("--ref2-x") << QString::number(refFieldDouble(ref2, kTranscriptSpeakerLocationXKey, 0.5), 'f', 8)
             << QStringLiteral("--ref2-y") << QString::number(refFieldDouble(ref2, kTranscriptSpeakerLocationYKey, 0.85), 'f', 8)
             << QStringLiteral("--ref2-box") << QString::number(qBound(0.01, refFieldDouble(ref2, kTranscriptSpeakerTrackingBoxSizeKey, 0.33), 1.0), 'f', 8)
             << QStringLiteral("--source-fps") << QString::number(clip.sourceFps > 0.0 ? clip.sourceFps : 30.0, 'f', 6)
             << QStringLiteral("--start-frame") << QString::number(qMax<int64_t>(0, startFrame))
             << QStringLiteral("--end-frame") << QString::number(qMax<int64_t>(0, endFrame))
             << QStringLiteral("--step") << QString::number(qMax(1, stepFrames));
        if (gpu) {
            args << QStringLiteral("--prefer-gpu");
        }
        return args;
    };

    {
        QDialog dialog;
        dialog.setWindowTitle(QStringLiteral("Docker AutoTrack Preflight"));
        dialog.resize(980, 620);

        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto* title = new QLabel(
            QStringLiteral("Review and adjust Docker AutoTrack settings before execution."),
            &dialog);
        title->setWordWrap(true);
        layout->addWidget(title);

        auto* preferGpuCheck = new QCheckBox(QStringLiteral("Prefer GPU execution (--gpus all), then fallback to CPU"), &dialog);
        preferGpuCheck->setChecked(preflight.preferGpu);
        layout->addWidget(preferGpuCheck);

        auto* autoBuildCheck = new QCheckBox(QStringLiteral("Auto-build image when missing"), &dialog);
        autoBuildCheck->setChecked(preflight.autoBuildMissingImage);
        layout->addWidget(autoBuildCheck);

        auto* forceRebuildCheck = new QCheckBox(QStringLiteral("Force rebuild image before run"), &dialog);
        forceRebuildCheck->setChecked(preflight.forceRebuildImage);
        layout->addWidget(forceRebuildCheck);

        auto* confirmRunCheck = new QCheckBox(QStringLiteral("Confirm the final docker run command before execution"), &dialog);
        confirmRunCheck->setChecked(preflight.confirmRunCommand);
        layout->addWidget(confirmRunCheck);

        auto* commandPreview = new QPlainTextEdit(&dialog);
        commandPreview->setReadOnly(true);
        commandPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
        commandPreview->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(commandPreview, 1);

        auto refreshPreview = [&]() {
            QStringList lines;
            lines << QStringLiteral("Image check:");
            lines << formatCommand(QStringLiteral("docker"), inspectArgs);
            lines << QString();
            lines << QStringLiteral("Image build:");
            lines << formatCommand(QStringLiteral("docker"), buildArgs);
            lines << QString();
            lines << QStringLiteral("Run command (GPU):");
            lines << formatCommand(QStringLiteral("docker"), buildRunArgs(true));
            lines << QString();
            lines << QStringLiteral("Run command (CPU):");
            lines << formatCommand(QStringLiteral("docker"), buildRunArgs(false));
            commandPreview->setPlainText(lines.join(QStringLiteral("\n")));
        };

        connect(forceRebuildCheck, &QCheckBox::toggled, &dialog, [&](bool checked) {
            if (checked && !autoBuildCheck->isChecked()) {
                autoBuildCheck->setChecked(true);
            }
        });
        connect(autoBuildCheck, &QCheckBox::toggled, &dialog, [&](bool checked) {
            if (!checked && forceRebuildCheck->isChecked()) {
                forceRebuildCheck->setChecked(false);
            }
        });
        refreshPreview();

        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
        auto* proceedButton = new QPushButton(QStringLiteral("Proceed"), &dialog);
        buttons->addWidget(cancelButton);
        buttons->addWidget(proceedButton);
        layout->addLayout(buttons);
        QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(proceedButton, &QPushButton::clicked, &dialog, &QDialog::accept);

        if (dialog.exec() != QDialog::Accepted) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled docker autotrack command");
            }
            return false;
        }

        preflight.preferGpu = preferGpuCheck->isChecked();
        preflight.autoBuildMissingImage = autoBuildCheck->isChecked();
        preflight.forceRebuildImage = forceRebuildCheck->isChecked();
        preflight.confirmRunCommand = confirmRunCheck->isChecked();
        s_preflightDefaults = preflight;
    }

    auto runDocker = [&](const QStringList& dockerArgs, int startTimeoutMs, QString* outputOut) -> bool {
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(dockerPath, dockerArgs);
        if (!proc.waitForStarted(startTimeoutMs)) {
            if (outputOut) {
                *outputOut = QStringLiteral("failed to start docker process");
            }
            return false;
        }
        proc.waitForFinished(-1);
        const QString output = QString::fromUtf8(proc.readAllStandardOutput());
        if (outputOut) {
            *outputOut = output;
        }
        return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    };
    auto runDockerInConsole = [&](const QStringList& dockerArgs,
                                  int startTimeoutMs,
                                  const QString& title,
                                  QString* outputOut) -> bool {
        QDialog dialog;
        dialog.setWindowTitle(title);
        dialog.resize(920, 560);

        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto* titleLabel = new QLabel(&dialog);
        titleLabel->setWordWrap(true);
        layout->addWidget(titleLabel);

        auto* output = new QPlainTextEdit(&dialog);
        output->setReadOnly(true);
        output->setLineWrapMode(QPlainTextEdit::NoWrap);
        output->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(output, 1);

        auto* autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), &dialog);
        autoScrollBox->setChecked(true);
        layout->addWidget(autoScrollBox);

        auto* inputRow = new QHBoxLayout;
        inputRow->setContentsMargins(0, 0, 0, 0);
        inputRow->setSpacing(8);
        auto* inputLabel = new QLabel(QStringLiteral("stdin"), &dialog);
        auto* inputLine = new QLineEdit(&dialog);
        inputLine->setPlaceholderText(QStringLiteral("Optional process input, then Send"));
        auto* sendButton = new QPushButton(QStringLiteral("Send"), &dialog);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), &dialog);
        closeButton->setEnabled(false);
        inputRow->addWidget(inputLabel);
        inputRow->addWidget(inputLine, 1);
        inputRow->addWidget(sendButton);
        inputRow->addWidget(closeButton);
        layout->addLayout(inputRow);

        QProcess process(&dialog);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setWorkingDirectory(QDir::currentPath());

        QString collectedOutput;
        const auto appendOutput = [&](const QString& text) {
            if (text.isEmpty()) {
                return;
            }
            collectedOutput += text;
            if (autoScrollBox->isChecked()) {
                output->moveCursor(QTextCursor::End);
            }
            output->insertPlainText(text);
            if (autoScrollBox->isChecked()) {
                output->moveCursor(QTextCursor::End);
            }
        };

        connect(&process, &QProcess::readyReadStandardOutput, &dialog, [&]() {
            appendOutput(QString::fromLocal8Bit(process.readAllStandardOutput()));
        });
        connect(&process, &QProcess::started, &dialog, [&]() {
            appendOutput(QStringLiteral("$ %1\n").arg(formatCommand(QStringLiteral("docker"), dockerArgs)));
        });
        connect(&process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
            appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error)));
            closeButton->setEnabled(true);
        });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
                [&](int exitCode, QProcess::ExitStatus exitStatus) {
                    appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                     .arg(exitCode)
                                     .arg(exitStatus == QProcess::NormalExit
                                              ? QStringLiteral("normal")
                                              : QStringLiteral("crashed")));
                    closeButton->setEnabled(true);
                });
        connect(sendButton, &QPushButton::clicked, &dialog, [&]() {
            const QString text = inputLine->text();
            if (text.isEmpty()) {
                return;
            }
            process.write(text.toUtf8());
            process.write("\n");
            appendOutput(QStringLiteral("> %1\n").arg(text));
            inputLine->clear();
        });
        connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
        connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        connect(&dialog, &QDialog::finished, &dialog, [&](int) {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(1000);
            }
        });

        titleLabel->setText(
            QStringLiteral("docker command\n%1").arg(formatCommand(QStringLiteral("docker"), dockerArgs)));
        process.start(dockerPath, dockerArgs);
        if (!process.waitForStarted(startTimeoutMs)) {
            appendOutput(QStringLiteral("[process error] failed to start docker process\n"));
            closeButton->setEnabled(true);
        }

        dialog.exec();
        if (outputOut) {
            *outputOut = collectedOutput;
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };

    // Ensure image is available locally; build from local Dockerfile when missing.
    {
        bool imageReady = false;
        if (!preflight.forceRebuildImage) {
            QString inspectOutput;
            imageReady = runDocker(inspectArgs, 10000, &inspectOutput);
        }
        if (!imageReady) {
            if (!preflight.autoBuildMissingImage) {
                if (errorOut) {
                    *errorOut = QStringLiteral(
                                    "docker image %1 was not available and auto-build is disabled in preflight settings")
                                    .arg(dockerImage.trimmed());
                }
                return false;
            }
            const QFileInfo dockerfileInfo(dockerfilePath);
            if (!dockerfileInfo.exists()) {
                if (errorOut) {
                    *errorOut = QStringLiteral(
                                    "docker image %1 not found and dockerfile missing: %2\n"
                                    "Set SPEAKER_AUTOTRACK_DOCKER_IMAGE to an existing image or provide SPEAKER_AUTOTRACK_DOCKERFILE.")
                                    .arg(dockerImage.trimmed(), dockerfilePath);
                }
                return false;
            }
            QString buildOutput;
            if (!runDocker(
                    QStringList{
                        QStringLiteral("build"),
                        QStringLiteral("-f"),
                        dockerfileInfo.absoluteFilePath(),
                        QStringLiteral("-t"),
                        dockerImage.trimmed(),
                        dockerBuildContext},
                    10000,
                    &buildOutput)) {
                if (errorOut) {
                    *errorOut = QStringLiteral(
                                    "docker image build failed for %1 using %2\n%3")
                                    .arg(dockerImage.trimmed(), dockerfileInfo.absoluteFilePath(), buildOutput.trimmed());
                }
                return false;
            }
        }
    }
    if (preflight.confirmRunCommand) {
        const QStringList gpuArgs = buildRunArgs(true);
        const QStringList cpuArgs = buildRunArgs(false);
        QString promptText;
        if (preflight.preferGpu) {
            promptText = QStringLiteral(
                "Docker AutoTrack is about to run these commands.\n\n"
                "Primary (GPU preferred):\n%1\n\n"
                "Fallback (CPU):\n%2\n\n"
                "Proceed?")
                             .arg(formatCommand(QStringLiteral("docker"), gpuArgs),
                                  formatCommand(QStringLiteral("docker"), cpuArgs));
        } else {
            promptText = QStringLiteral(
                "Docker AutoTrack is about to run this command:\n\n%1\n\nProceed?")
                             .arg(formatCommand(QStringLiteral("docker"), cpuArgs));
        }
        const QMessageBox::StandardButton approved = QMessageBox::question(
            nullptr,
            QStringLiteral("Approve Docker AutoTrack Command"),
            promptText,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (approved != QMessageBox::Yes) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled docker autotrack command");
            }
            return false;
        }
    }

    QString output;
    if (preflight.preferGpu && !runDockerInConsole(
                         buildRunArgs(true),
                         10000,
                         QStringLiteral("Speaker AutoTrack (Docker GPU)"),
                         &output)) {
        // GPU-first policy: if GPU path fails, fall back to CPU automatically.
        output.clear();
    }
    if (output.isEmpty() && !runDockerInConsole(
                                  buildRunArgs(false),
                                  10000,
                                  QStringLiteral("Speaker AutoTrack (Docker CPU Fallback)"),
                                  &output)) {
        if (errorOut) {
            *errorOut = QStringLiteral("docker autotrack failed:\n%1")
                            .arg(output.trimmed());
        }
        return false;
    }

    QFile outJson(outputFile);
    if (!outJson.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("autotrack output JSON not found");
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(outJson.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (errorOut) {
            *errorOut = QStringLiteral("autotrack output JSON invalid");
        }
        return false;
    }

    const QJsonArray arr = doc.array();
    QJsonArray keyframes;
    for (const QJsonValue& value : arr) {
        const QJsonObject in = value.toObject();
        if (in.isEmpty()) {
            continue;
        }
        const int64_t frame = in.value(QStringLiteral("frame")).toVariant().toLongLong();
        const double x = in.value(QStringLiteral("x")).toDouble(-1.0);
        const double y = in.value(QStringLiteral("y")).toDouble(-1.0);
        if (frame < 0 || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
            continue;
        }
        QJsonObject out;
        out[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        out[QString(kTranscriptSpeakerLocationXKey)] = x;
        out[QString(kTranscriptSpeakerLocationYKey)] = y;
        const double confidence = qBound(0.0, in.value(QStringLiteral("confidence")).toDouble(0.0), 1.0);
        out[QString(kTranscriptSpeakerTrackingConfidenceKey)] = confidence;
        const double boxSize = in.value(QStringLiteral("box_size")).toDouble(-1.0);
        const double boxLeft = in.value(QStringLiteral("box_left")).toDouble(-1.0);
        const double boxTop = in.value(QStringLiteral("box_top")).toDouble(-1.0);
        const double boxRight = in.value(QStringLiteral("box_right")).toDouble(-1.0);
        const double boxBottom = in.value(QStringLiteral("box_bottom")).toDouble(-1.0);
        const bool hasCorners =
            boxLeft >= 0.0 && boxTop >= 0.0 && boxRight >= boxLeft && boxBottom >= boxTop &&
            boxRight <= 1.0 && boxBottom <= 1.0;
        if (boxSize > 0.0) {
            const double normalizedSize = qBound(0.01, boxSize, 1.0);
            out[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = normalizedSize;
            if (hasCorners) {
                out[QString(kTranscriptSpeakerTrackingBoxLeftKey)] = boxLeft;
                out[QString(kTranscriptSpeakerTrackingBoxTopKey)] = boxTop;
                out[QString(kTranscriptSpeakerTrackingBoxRightKey)] = boxRight;
                out[QString(kTranscriptSpeakerTrackingBoxBottomKey)] = boxBottom;
            } else {
                writeNormalizedFaceBox(out, x, y, normalizedSize);
            }
        }
        const QString source = in.value(QStringLiteral("source")).toString().trimmed();
        out[QString(kTranscriptSpeakerTrackingSourceKey)] =
            source.isEmpty() ? QStringLiteral("autotrack_docker_v1") : source;
        keyframes.push_back(out);
    }

    if (keyframes.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("autotrack produced no keyframes");
        }
        return false;
    }
    *keyframesOut = keyframes;
    return true;
}
