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

void SpeakersTab::updateSelectedSpeakerPanel()
{
    if (!m_widgets.selectedSpeakerIdLabel &&
        !m_widgets.selectedSpeakerRef1ImageLabel &&
        !m_widgets.selectedSpeakerRef2ImageLabel) {
        return;
    }

    const QString speakerId = selectedSpeakerId();
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (speakerId.isEmpty() || !clip || !m_loadedTranscriptDoc.isObject()) {
        if (m_widgets.selectedSpeakerIdLabel) {
            m_widgets.selectedSpeakerIdLabel->setText(QStringLiteral("No speaker selected"));
        }
        if (m_widgets.speakerCurrentSentenceLabel) {
            m_widgets.speakerCurrentSentenceLabel->setText(
                QStringLiteral("Select a speaker to view sentence context."));
        }
        if (m_widgets.selectedSpeakerRef1ImageLabel) {
            m_widgets.selectedSpeakerRef1ImageLabel->setPixmap(unsetSpeakerAvatar(120));
            m_widgets.selectedSpeakerRef1ImageLabel->setToolTip(QStringLiteral("Ref1 unset"));
        }
        if (m_widgets.selectedSpeakerRef2ImageLabel) {
            m_widgets.selectedSpeakerRef2ImageLabel->setPixmap(unsetSpeakerAvatar(120));
            m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(QStringLiteral("Ref2 unset"));
        }
        updateSpeakerFramingTargetControls();
        return;
    }

    const QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QString displayName = profile.value(QString(kTranscriptSpeakerNameKey)).toString(speakerId);
    if (m_widgets.selectedSpeakerIdLabel) {
        m_widgets.selectedSpeakerIdLabel->setText(
            QStringLiteral("%1 (%2)").arg(displayName, speakerId));
    }
    if (m_widgets.speakerCurrentSentenceLabel) {
        m_widgets.speakerCurrentSentenceLabel->setText(
            currentSpeakerSentenceAtCurrentFrame(speakerId));
    }

    const QJsonObject tracking = speakerFramingObject(profile);
    const QJsonObject ref1Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef1Key)).toObject();
    const QJsonObject ref2Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef2Key)).toObject();
    if (m_widgets.selectedSpeakerRef1ImageLabel) {
        m_widgets.selectedSpeakerRef1ImageLabel->setPixmap(speakerReferenceAvatar(*clip, speakerId, ref1Obj, 120));
        m_widgets.selectedSpeakerRef1ImageLabel->setToolTip(
            ref1Obj.isEmpty()
                ? QStringLiteral("Ref1 unset")
                : QStringLiteral("Ref1 set. Drag to reposition, wheel to zoom crop."));
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->setPixmap(speakerReferenceAvatar(*clip, speakerId, ref2Obj, 120));
        m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(
            ref2Obj.isEmpty()
                ? QStringLiteral("Ref2 unset")
                : QStringLiteral("Ref2 set. Drag to reposition, wheel to zoom crop."));
    }
    updateSpeakerFramingTargetControls();
}

void SpeakersTab::updateSpeakerFramingTargetControls()
{
    if (!m_widgets.speakerFramingTargetXSpin &&
        !m_widgets.speakerFramingTargetYSpin &&
        !m_widgets.speakerFramingTargetBoxSpin &&
        !m_widgets.speakerFramingZoomEnabledCheckBox) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    m_updatingSpeakerFramingTargetControls = true;
    if (m_widgets.speakerFramingTargetXSpin) {
        QSignalBlocker blocker(m_widgets.speakerFramingTargetXSpin);
        m_widgets.speakerFramingTargetXSpin->setValue(
            clip ? qBound<qreal>(0.0, clip->speakerFramingTargetXNorm, 1.0) : 0.5);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        QSignalBlocker blocker(m_widgets.speakerFramingTargetYSpin);
        m_widgets.speakerFramingTargetYSpin->setValue(
            clip ? qBound<qreal>(0.0, clip->speakerFramingTargetYNorm, 1.0) : 0.35);
    }
    const qreal boxValue = clip
        ? qBound<qreal>(-1.0, clip->speakerFramingTargetBoxNorm, 1.0)
        : -1.0;
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        QSignalBlocker blocker(m_widgets.speakerFramingZoomEnabledCheckBox);
        m_widgets.speakerFramingZoomEnabledCheckBox->setChecked(boxValue > 0.0);
    }
    if (m_widgets.speakerFramingTargetBoxSpin) {
        QSignalBlocker blocker(m_widgets.speakerFramingTargetBoxSpin);
        m_widgets.speakerFramingTargetBoxSpin->setValue(boxValue > 0.0 ? boxValue : 0.20);
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        QSignalBlocker blocker(m_widgets.speakerApplyFramingToClipCheckBox);
        m_widgets.speakerApplyFramingToClipCheckBox->setChecked(clip ? clip->speakerFramingEnabled : false);
    }
    if (m_widgets.speakerClipFramingStatusLabel) {
        const bool enabled = clip ? clip->speakerFramingEnabled : false;
        const int keyCount = clip ? clip->speakerFramingKeyframes.size() : 0;
        m_widgets.speakerClipFramingStatusLabel->setText(
            QStringLiteral("Face Stabilize: %1 | %2 keys")
                .arg(enabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
                .arg(keyCount));
    }
    m_updatingSpeakerFramingTargetControls = false;
}

bool SpeakersTab::saveClipSpeakerFramingTargetsFromControls()
{
    if (m_updatingSpeakerFramingTargetControls || !m_speakerDeps.updateClipById) {
        return false;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        return false;
    }
    const bool zoomEnabled = m_widgets.speakerFramingZoomEnabledCheckBox
        ? m_widgets.speakerFramingZoomEnabledCheckBox->isChecked()
        : false;
    const qreal bakedTargetX = qBound<qreal>(0.0, selectedClip->speakerFramingBakedTargetXNorm, 1.0);
    const qreal bakedTargetY = qBound<qreal>(0.0, selectedClip->speakerFramingBakedTargetYNorm, 1.0);
    const qreal targetX = m_widgets.speakerFramingTargetXSpin
        ? qBound<qreal>(0.0, m_widgets.speakerFramingTargetXSpin->value(), 1.0)
        : qBound<qreal>(0.0, selectedClip->speakerFramingTargetXNorm, 1.0);
    const qreal targetY = m_widgets.speakerFramingTargetYSpin
        ? qBound<qreal>(0.0, m_widgets.speakerFramingTargetYSpin->value(), 1.0)
        : qBound<qreal>(0.0, selectedClip->speakerFramingTargetYNorm, 1.0);
    const qreal targetBox = zoomEnabled && m_widgets.speakerFramingTargetBoxSpin
        ? qBound<qreal>(0.01, m_widgets.speakerFramingTargetBoxSpin->value(), 1.0)
        : -1.0;
    const QSize outputSize = m_speakerDeps.getOutputSize && m_speakerDeps.getOutputSize().isValid()
        ? m_speakerDeps.getOutputSize()
        : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(outputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(outputSize.height()));
    const qreal bakedTargetBox = qBound<qreal>(-1.0, selectedClip->speakerFramingBakedTargetBoxNorm, 1.0);
    const qreal bakedTargetXPx = bakedTargetX * outputWidth;
    const qreal bakedTargetYPx = bakedTargetY * outputHeight;
    const qreal targetXPx = targetX * outputWidth;
    const qreal targetYPx = targetY * outputHeight;

    const QString mediaPathCandidate = interactivePreviewMediaPathForClip(*selectedClip);
    const QString mediaPath = QFileInfo::exists(mediaPathCandidate) ? mediaPathCandidate : selectedClip->filePath;
    const MediaProbeResult probe = probeMediaFile(mediaPath, 4.0);
    const QRect fittedRect = fitRectForSourceInOutput(
        probe.frameSize.isValid() ? probe.frameSize : outputSize,
        outputSize);
    const qreal fittedCenterX = static_cast<qreal>(fittedRect.center().x());
    const qreal fittedCenterY = static_cast<qreal>(fittedRect.center().y());

    qreal scaleFactor = 1.0;
    if (targetBox > 0.0 && bakedTargetBox > 0.0) {
        scaleFactor = qMax<qreal>(0.01, targetBox / bakedTargetBox);
    }
    const bool shouldRetargetFramingKeys =
        (!qFuzzyCompare(targetXPx + 1.0, bakedTargetXPx + 1.0) ||
         !qFuzzyCompare(targetYPx + 1.0, bakedTargetYPx + 1.0) ||
         !qFuzzyCompare(scaleFactor + 1.0, 2.0));

    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        if (shouldRetargetFramingKeys && !editableClip.speakerFramingKeyframes.isEmpty()) {
            // Retarget solved framing keys to the new FaceBox target using a stable geometric transform.
            for (TimelineClip::TransformKeyframe& keyframe : editableClip.speakerFramingKeyframes) {
                const qreal oldTranslationX = keyframe.translationX;
                const qreal oldTranslationY = keyframe.translationY;
                keyframe.translationX =
                    targetXPx - fittedCenterX -
                    (scaleFactor * (bakedTargetXPx - fittedCenterX - oldTranslationX));
                keyframe.translationY =
                    targetYPx - fittedCenterY -
                    (scaleFactor * (bakedTargetYPx - fittedCenterY - oldTranslationY));
                keyframe.scaleX = sanitizeScaleValue(keyframe.scaleX * scaleFactor);
                keyframe.scaleY = sanitizeScaleValue(keyframe.scaleY * scaleFactor);
            }
        }
        editableClip.speakerFramingTargetXNorm = targetX;
        editableClip.speakerFramingTargetYNorm = targetY;
        editableClip.speakerFramingTargetBoxNorm = targetBox;
        editableClip.speakerFramingBakedTargetXNorm = targetX;
        editableClip.speakerFramingBakedTargetYNorm = targetY;
        editableClip.speakerFramingBakedTargetBoxNorm = targetBox;
        normalizeClipTransformKeyframes(editableClip);
    });
    if (changed && m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    return changed;
}

bool SpeakersTab::saveClipSpeakerFramingEnabledFromControls()
{
    if (m_updatingSpeakerFramingTargetControls || !m_speakerDeps.updateClipById ||
        !m_widgets.speakerApplyFramingToClipCheckBox) {
        return false;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        return false;
    }
    const bool requestedEnabled = m_widgets.speakerApplyFramingToClipCheckBox->isChecked();
    const bool hasRuntimeBinding = !selectedClip->speakerFramingSpeakerId.trimmed().isEmpty();
    if (requestedEnabled && selectedClip->speakerFramingKeyframes.isEmpty() && !hasRuntimeBinding) {
        return false;
    }
    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        editableClip.speakerFramingEnabled = requestedEnabled;
    });
    if (changed && m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    return changed;
}

QString SpeakersTab::currentSpeakerSentenceAtCurrentFrame(const QString& speakerId) const
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return QStringLiteral("No sentence available.");
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return QStringLiteral("No sentence available.");
    }
    const int64_t sourceFrame = currentSourceFrameForClip(*clip);
    const QString targetSpeaker = speakerId.trimmed();

    struct SentenceRun {
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        QString text;
    };
    QVector<SentenceRun> runs;

    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();

        bool runActive = false;
        SentenceRun run;
        QStringList runWords;
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                if (runActive && !runWords.isEmpty()) {
                    run.text = runWords.join(QStringLiteral(" "));
                    runs.push_back(run);
                }
                runActive = false;
                runWords.clear();
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (wordText.isEmpty() || startSeconds < 0.0 || endSeconds < startSeconds) {
                if (runActive && !runWords.isEmpty()) {
                    run.text = runWords.join(QStringLiteral(" "));
                    runs.push_back(run);
                }
                runActive = false;
                runWords.clear();
                continue;
            }
            const int64_t wordStartFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t wordEndFrame =
                qMax<int64_t>(wordStartFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            if (wordSpeaker != targetSpeaker) {
                if (runActive && !runWords.isEmpty()) {
                    run.text = runWords.join(QStringLiteral(" "));
                    runs.push_back(run);
                }
                runActive = false;
                runWords.clear();
                continue;
            }

            if (!runActive) {
                run = SentenceRun{};
                run.startFrame = wordStartFrame;
                run.endFrame = wordEndFrame;
                runWords.clear();
                runWords.push_back(wordText);
                runActive = true;
            } else {
                run.endFrame = qMax<int64_t>(run.endFrame, wordEndFrame);
                runWords.push_back(wordText);
            }
        }
        if (runActive && !runWords.isEmpty()) {
            run.text = runWords.join(QStringLiteral(" "));
            runs.push_back(run);
        }
    }

    if (runs.isEmpty()) {
        return QStringLiteral("No sentence found for this speaker.");
    }

    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < runs.size(); ++i) {
        const SentenceRun& run = runs.at(i);
        if (sourceFrame >= run.startFrame && sourceFrame <= run.endFrame) {
            bestIndex = i;
            break;
        }
        const int64_t distance =
            (sourceFrame < run.startFrame)
                ? (run.startFrame - sourceFrame)
                : (sourceFrame - run.endFrame);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    if (bestIndex < 0 || bestIndex >= runs.size()) {
        return QStringLiteral("No sentence found for this speaker.");
    }
    return runs.at(bestIndex).text;
}

int64_t SpeakersTab::currentSourceFrameForClip(const TimelineClip& clip) const
{
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip.startFrame;
    const QVector<RenderSyncMarker> markers =
        m_speakerDeps.getRenderSyncMarkers
            ? m_speakerDeps.getRenderSyncMarkers()
            : QVector<RenderSyncMarker>{};
    return transcriptFrameForClipAtTimelineSample(clip, frameToSamples(timelineFrame), markers);
}

QVector<int64_t> SpeakersTab::speakerSourceFrames(const QJsonObject& transcriptRoot,
                                                  const QString& speakerId) const
{
    QVector<int64_t> frames;
    const QString targetSpeaker = speakerId.trimmed();
    if (targetSpeaker.isEmpty()) {
        return frames;
    }
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        bool sentenceActive = false;
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                sentenceActive = false;
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const bool validWord = !wordText.isEmpty() && startSeconds >= 0.0;
            if (wordSpeaker != targetSpeaker || !validWord) {
                sentenceActive = false;
                continue;
            }
            if (!sentenceActive) {
                frames.push_back(
                    qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps))));
                sentenceActive = true;
            }
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

bool SpeakersTab::saveSpeakerProfileEdit(int tableRow, int column, const QString& valueText)
{
    if (!m_widgets.speakersTable || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    QTableWidgetItem* idItem = m_widgets.speakersTable->item(tableRow, 1);
    if (!idItem) {
        return false;
    }
    const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
    if (speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();

    if (column == 2) {
        profile[QString(kTranscriptSpeakerNameKey)] = valueText.trimmed().isEmpty() ? speakerId : valueText.trimmed();
    } else if (column == 3 || column == 4) {
        bool ok = false;
        const double parsed = valueText.toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double bounded = qBound(0.0, parsed, 1.0);
        if (column == 3) {
            location[QString(kTranscriptSpeakerLocationXKey)] = bounded;
        } else {
            location[QString(kTranscriptSpeakerLocationYKey)] = bounded;
        }
        profile[QString(kTranscriptSpeakerLocationKey)] = location;
    } else {
        return false;
    }

    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        return false;
    }
    return true;
}

bool SpeakersTab::saveSpeakerTrackingReferenceAt(const QString& speakerId,
                                                 int referenceIndex,
                                                 int64_t frame,
                                                 qreal xNorm,
                                                 qreal yNorm,
                                                 qreal boxSizeNorm)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
    const double x = qBound(0.0, static_cast<double>(xNorm), 1.0);
    const double y = qBound(0.0, static_cast<double>(yNorm), 1.0);
    location[QString(kTranscriptSpeakerLocationXKey)] = x;
    location[QString(kTranscriptSpeakerLocationYKey)] = y;
    profile[QString(kTranscriptSpeakerLocationKey)] = location;

    QJsonObject tracking = speakerFramingObject(profile);
    const bool previouslyEnabled =
        tracking.value(QString(kTranscriptSpeakerTrackingEnabledKey)).toBool(false);
    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = previouslyEnabled;
    tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("refs_updated");

    QJsonObject refObj;
    refObj[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
    refObj[QString(kTranscriptSpeakerLocationXKey)] = x;
    refObj[QString(kTranscriptSpeakerLocationYKey)] = y;
    if (boxSizeNorm > 0.0) {
        const qreal normalizedSize = qBound(0.01, static_cast<double>(boxSizeNorm), 1.0);
        refObj[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = normalizedSize;
        writeNormalizedFaceBox(refObj, xNorm, yNorm, normalizedSize);
    }
    if (referenceIndex == 1) {
        tracking[QString(kTranscriptSpeakerTrackingRef1Key)] = refObj;
    } else {
        tracking[QString(kTranscriptSpeakerTrackingRef2Key)] = refObj;
    }

    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::saveSpeakerTrackingReference(const QString& speakerId, int referenceIndex)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t frame = currentSourceFrameForClip(*clip);

    QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
    const qreal x = qBound(0.0, location.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal y = qBound(0.0, location.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    return saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, x, y);
}

bool SpeakersTab::armReferencePickForSpeaker(const QString& speakerId, int referenceIndex)
{
    if (!activeCutMutable() || speakerId.trimmed().isEmpty() || (referenceIndex != 1 && referenceIndex != 2)) {
        return false;
    }
    m_lastSelectedSpeakerIdHint = speakerId.trimmed();
    m_pendingReferencePick = referenceIndex;
    updateSpeakerTrackingStatusLabel();
    return true;
}

bool SpeakersTab::clearSpeakerTrackingReferences(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    profile.remove(QString(kTranscriptSpeakerFramingKey));
    profile.remove(QString(kTranscriptSpeakerTrackingKey));
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::setSpeakerTrackingEnabled(const QString& speakerId, bool enabled)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return false;
    }
    if (enabled && !transcriptTrackingHasPointstream(tracking)) {
        return false;
    }
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = enabled;
    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::deleteSpeakerAutoTrackPointstream(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return false;
    }
    if (!transcriptTrackingHasPointstream(tracking)) {
        return true;
    }

    tracking[QString(kTranscriptSpeakerTrackingKeyframesKey)] = QJsonArray();
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("deleted_pointstream");
    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::setSpeakerSkipped(const QString& speakerId, bool skipped)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    const QString targetSpeaker = speakerId.trimmed();
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    bool changed = false;

    for (int segIndex = 0; segIndex < segments.size(); ++segIndex) {
        QJsonObject segmentObj = segments.at(segIndex).toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            QJsonObject wordObj = words.at(wordIndex).toObject();
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
            const bool previous = wordObj.value(QStringLiteral("skipped")).toBool(false);
            if (previous != skipped) {
                wordObj[QStringLiteral("skipped")] = skipped;
                words.replace(wordIndex, wordObj);
                changed = true;
            }
        }
        segmentObj[QStringLiteral("words")] = words;
        segments.replace(segIndex, segmentObj);
    }

    if (!changed) {
        return true;
    }

    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);
    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::selectedSpeakerReferenceObject(int referenceIndex,
                                                 QString* speakerIdOut,
                                                 QJsonObject* refOut) const
{
    if (referenceIndex != 1 && referenceIndex != 2) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject framing = speakerFramingObject(profile);
    const QJsonObject refObj = framing.value(
        QString(referenceIndex == 1 ? kTranscriptSpeakerTrackingRef1Key
                                    : kTranscriptSpeakerTrackingRef2Key)).toObject();
    if (refObj.isEmpty()) {
        return false;
    }
    if (speakerIdOut) {
        *speakerIdOut = speakerId;
    }
    if (refOut) {
        *refOut = refObj;
    }
    return true;
}

bool SpeakersTab::adjustSelectedReferenceAvatarZoom(int referenceIndex, int wheelDelta)
{
    if (!activeCutMutable() || referenceIndex < 1 || referenceIndex > 2 || wheelDelta == 0) {
        return false;
    }
    QString speakerId;
    QJsonObject refObj;
    if (!selectedSpeakerReferenceObject(referenceIndex, &speakerId, &refObj)) {
        return false;
    }

    const qreal units = static_cast<qreal>(wheelDelta) / 120.0;
    if (std::abs(units) < 0.001) {
        return false;
    }
    const qreal currentBoxSize = qBound<qreal>(
        0.05, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(1.0 / 3.0), 1.0);
    // Scroll up zooms in (smaller source crop), scroll down zooms out.
    const qreal nextBoxSize = qBound<qreal>(0.05, currentBoxSize - (units * 0.02), 1.0);
    if (std::abs(nextBoxSize - currentBoxSize) < 0.0005) {
        return false;
    }

    const int64_t frame =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal xNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal yNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    if (!saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, xNorm, yNorm, nextBoxSize)) {
        return false;
    }

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

QPointF SpeakersTab::referenceNormPerPixelFromSourceFrame(const TimelineClip& clip,
                                                          const QJsonObject& refObj,
                                                          int avatarSize) const
{
    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));

    editor::DecoderContext ctx(interactivePreviewMediaPathForClip(clip));
    if (!ctx.initialize()) {
        return QPointF(0.0, 0.0);
    }
    const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
    const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return QPointF(0.0, 0.0);
    }

    const int width = image.width();
    const int height = image.height();
    const int minSide = qMin(width, height);
    int side = qMax(40, minSide / 3);
    if (boxSizeNorm > 0.0) {
        side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
    }
    const int displaySize = qMax(1, avatarSize);
    const qreal sourcePxPerAvatarPx = static_cast<qreal>(side) / qMax<qreal>(1.0, displaySize);
    return QPointF(sourcePxPerAvatarPx / qMax<qreal>(1.0, width),
                   sourcePxPerAvatarPx / qMax<qreal>(1.0, height));
}

bool SpeakersTab::beginSelectedReferenceAvatarDrag(int referenceIndex, const QPoint& localPos)
{
    if (!activeCutMutable() || referenceIndex < 1 || referenceIndex > 2) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    QString speakerId;
    QJsonObject refObj;
    if (!selectedSpeakerReferenceObject(referenceIndex, &speakerId, &refObj)) {
        return false;
    }
    const QPointF normPerPixel = referenceNormPerPixelFromSourceFrame(*clip, refObj, 120);
    if (normPerPixel.x() <= 0.0 || normPerPixel.y() <= 0.0) {
        return false;
    }
    m_selectedAvatarDragActive = true;
    m_selectedAvatarDragReferenceIndex = referenceIndex;
    m_selectedAvatarDragSpeakerId = speakerId;
    m_selectedAvatarDragLastPos = localPos;
    m_selectedAvatarDragRefObj = refObj;
    m_selectedAvatarDragNormPerPixel = normPerPixel;
    return true;
}

void SpeakersTab::updateSelectedReferenceAvatarDrag(const QPoint& localPos)
{
    if (!m_selectedAvatarDragActive) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }
    const QPoint delta = localPos - m_selectedAvatarDragLastPos;
    m_selectedAvatarDragLastPos = localPos;

    const qreal currentX = qBound<qreal>(0.0, m_selectedAvatarDragRefObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal currentY = qBound<qreal>(0.0, m_selectedAvatarDragRefObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal nextX = qBound<qreal>(0.0, currentX + (delta.x() * m_selectedAvatarDragNormPerPixel.x()), 1.0);
    const qreal nextY = qBound<qreal>(0.0, currentY + (delta.y() * m_selectedAvatarDragNormPerPixel.y()), 1.0);
    m_selectedAvatarDragRefObj[QString(kTranscriptSpeakerLocationXKey)] = nextX;
    m_selectedAvatarDragRefObj[QString(kTranscriptSpeakerLocationYKey)] = nextY;

    QLabel* targetLabel = m_selectedAvatarDragReferenceIndex == 1
        ? m_widgets.selectedSpeakerRef1ImageLabel
        : m_widgets.selectedSpeakerRef2ImageLabel;
    if (targetLabel) {
        targetLabel->setPixmap(
            speakerReferenceAvatar(*clip, m_selectedAvatarDragSpeakerId, m_selectedAvatarDragRefObj, 120));
    }
}

void SpeakersTab::finishSelectedReferenceAvatarDrag(bool commit)
{
    if (!m_selectedAvatarDragActive) {
        return;
    }
    const QString speakerId = m_selectedAvatarDragSpeakerId;
    const int referenceIndex = m_selectedAvatarDragReferenceIndex;
    const QJsonObject refObj = m_selectedAvatarDragRefObj;

    m_selectedAvatarDragActive = false;
    m_selectedAvatarDragReferenceIndex = 0;
    m_selectedAvatarDragSpeakerId.clear();
    m_selectedAvatarDragLastPos = QPoint();
    m_selectedAvatarDragRefObj = QJsonObject();
    m_selectedAvatarDragNormPerPixel = QPointF();

    if (!commit || !activeCutMutable() || speakerId.isEmpty()) {
        refresh();
        return;
    }

    const int64_t frame =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal xNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal yNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
    if (!saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, xNorm, yNorm, boxSizeNorm)) {
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

void SpeakersTab::onSpeakersTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || !activeCutMutable()) {
        return;
    }
    if (!saveSpeakerProfileEdit(item->row(), item->column(), item->text())) {
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
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakersSelectionChanged()
{
    if (m_updating) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const QString clipId = clip ? clip->id : QString();
    const QString speakerId = selectedSpeakerId();
    if (!speakerId.isEmpty()) {
        m_lastSelectedSpeakerIdHint = speakerId;
        const bool selectionChanged =
            (speakerId != m_lastSelectionSeekSpeakerId) || (clipId != m_lastSelectionSeekClipId);
        if (selectionChanged) {
            seekToSpeakerFirstWord(speakerId);
            m_lastSelectionSeekSpeakerId = speakerId;
            m_lastSelectionSeekClipId = clipId;
        }
    } else {
        m_lastSelectionSeekSpeakerId.clear();
        m_lastSelectionSeekClipId.clear();
    }
    updateSpeakerTrackingStatusLabel();
    updateSelectedSpeakerPanel();
}

void SpeakersTab::onSpeakersTableContextMenuRequested(const QPoint& pos)
{
    if (!m_widgets.speakersTable || !m_loadedTranscriptDoc.isObject()) {
        return;
    }

    const int row = m_widgets.speakersTable->itemAt(pos)
        ? m_widgets.speakersTable->itemAt(pos)->row()
        : m_widgets.speakersTable->currentRow();
    if (row < 0) {
        return;
    }

    QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
    if (!idItem) {
        return;
    }
    const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
    if (speakerId.isEmpty()) {
        return;
    }

    if (!m_widgets.speakersTable->selectionModel()->isRowSelected(
            row, QModelIndex())) {
        m_widgets.speakersTable->clearSelection();
        m_widgets.speakersTable->selectRow(row);
    }
    if (m_widgets.speakersTable->currentRow() != row) {
        m_widgets.speakersTable->setCurrentCell(row, 1);
    }
    QStringList selectedSpeakerIds;
    if (m_widgets.speakersTable->selectionModel()) {
        const QModelIndexList selectedRows =
            m_widgets.speakersTable->selectionModel()->selectedRows();
        for (const QModelIndex& index : selectedRows) {
            if (!index.isValid()) {
                continue;
            }
            QTableWidgetItem* selectedIdItem =
                m_widgets.speakersTable->item(index.row(), 1);
            if (!selectedIdItem) {
                continue;
            }
            const QString selectedSpeakerId =
                selectedIdItem->data(Qt::UserRole).toString().trimmed();
            if (!selectedSpeakerId.isEmpty()) {
                selectedSpeakerIds.push_back(selectedSpeakerId);
            }
        }
    }
    if (selectedSpeakerIds.isEmpty()) {
        selectedSpeakerIds.push_back(speakerId);
    }
    selectedSpeakerIds.removeDuplicates();

    int wordCount = 0;
    int skippedCount = 0;
    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (wordSpeaker != speakerId) {
                continue;
            }
            if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                continue;
            }
            ++wordCount;
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                ++skippedCount;
            }
        }
    }

    QMenu menu(m_widgets.speakersTable);
    QAction* skipAction = menu.addAction(QStringLiteral("Skip Speaker"));
    QAction* unskipAction = menu.addAction(QStringLiteral("Unskip Speaker"));
    menu.addSeparator();
    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);
    const bool hasTrackingModel = transcriptTrackingHasPointstream(tracking);
    bool hasRef1 = false;
    bool hasRef2 = false;
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    const bool hasAnyRef = hasRef1 || hasRef2;
    const bool canMutate = activeCutMutable();

    QAction* enableTrackingAction = menu.addAction(QStringLiteral("Enable Subtitle Face Tracking"));
    QAction* disableTrackingAction = menu.addAction(QStringLiteral("Disable Subtitle Face Tracking"));
    QMenu* autoTrackMenu = menu.addMenu(QStringLiteral("BoxStream"));
    QAction* runAutoTrackAction = nullptr;
    QAction* deleteAutoTrackAction = nullptr;
    runAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("Generate BoxStream"));
    deleteAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("Delete BoxStream"));
    menu.addSeparator();
    const QString exportLabel = selectedSpeakerIds.size() > 1
        ? QStringLiteral("Export Video (%1 Speakers)").arg(selectedSpeakerIds.size())
        : QStringLiteral("Export Video");
    QAction* exportVideoAction = menu.addAction(exportLabel);

    skipAction->setEnabled(canMutate && wordCount > 0 && skippedCount < wordCount);
    unskipAction->setEnabled(canMutate && wordCount > 0 && skippedCount > 0);
    enableTrackingAction->setEnabled(canMutate && hasTrackingModel && !trackingEnabled);
    disableTrackingAction->setEnabled(canMutate && trackingEnabled);
    if (runAutoTrackAction) {
        runAutoTrackAction->setEnabled(canMutate && hasAnyRef);
    }
    if (deleteAutoTrackAction) {
        deleteAutoTrackAction->setEnabled(canMutate && hasTrackingModel);
    }
    exportVideoAction->setEnabled(m_speakerDeps.exportSpeakersVideo &&
                                  !selectedSpeakerIds.isEmpty());

    QAction* chosen = menu.exec(m_widgets.speakersTable->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (chosen == exportVideoAction) {
        if (m_speakerDeps.exportSpeakersVideo) {
            m_speakerDeps.exportSpeakersVideo(selectedSpeakerIds);
        }
        return;
    }
    if (!activeCutMutable()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Speakers"),
            QStringLiteral("Speaker actions are editable only on derived cuts (not Original)."));
        return;
    }
    if (wordCount <= 0) {
        if (chosen != enableTrackingAction &&
            chosen != disableTrackingAction &&
            chosen != runAutoTrackAction &&
            chosen != deleteAutoTrackAction) {
            return;
        }
    }

    if (chosen == runAutoTrackAction) {
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
        return;
    }

    if (chosen == deleteAutoTrackAction) {
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
        return;
    }

    if (chosen == enableTrackingAction || chosen == disableTrackingAction) {
        const bool enable = (chosen == enableTrackingAction);
        if (!setSpeakerTrackingEnabled(speakerId, enable)) {
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
        return;
    }

    const bool skip = (chosen == skipAction);
    const QString actionLabel = skip ? QStringLiteral("skip") : QStringLiteral("unskip");
    const auto confirmation = QMessageBox::question(
        nullptr,
        QStringLiteral("Confirm Speaker Skip"),
        QStringLiteral("Do you want to %1 all transcript words for speaker '%2' in this cut?")
            .arg(actionLabel, speakerId));
    if (confirmation != QMessageBox::Yes) {
        return;
    }

    if (!setSpeakerSkipped(speakerId, skip)) {
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

void SpeakersTab::onSpeakersTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_widgets.speakersTable) {
        return;
    }

    const int column = item->column();
    const QString clickedSpeakerId = item->data(Qt::UserRole).toString().trimmed();
    if (!clickedSpeakerId.isEmpty() && m_widgets.speakersTable->currentRow() != item->row()) {
        m_widgets.speakersTable->setCurrentCell(item->row(), 1);
    }
    if (column == 5 && activeCutMutable()) {
        const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
        if (speakerId.isEmpty()) {
            updateSpeakerTrackingStatusLabel();
            return;
        }
        if (!cycleFramingModeForSpeaker(speakerId)) {
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
        return;
    }
    if ((column == 0 || column == 6 || column == 7) && activeCutMutable()) {
        const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
        if (speakerId.isEmpty()) {
            updateSpeakerTrackingStatusLabel();
            return;
        }
        armReferencePickForSpeaker(speakerId, (column == 7) ? 2 : 1);
        return;
    }

    // Preserve in-place editing workflow for editable columns.
    if (column == 2 || column == 3 || column == 4) {
        return;
    }
    const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
    if (!speakerId.isEmpty()) {
        // Selecting a speaker row starts the "locate face" workflow.
        // The first picked reference becomes the basis for future framing.
        armReferencePickForSpeaker(speakerId, 1);
        seekToSpeakerFirstWord(speakerId);
    }
}

void SpeakersTab::seekToSpeakerFirstWord(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject() || !m_deps.seekToTimelineFrame) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }

    const QString targetSpeaker = speakerId.trimmed();
    if (targetSpeaker.isEmpty()) {
        return;
    }

    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    double earliestStartSeconds = std::numeric_limits<double>::max();
    bool found = false;
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
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            if (startSeconds < 0.0) {
                continue;
            }
            if (startSeconds < earliestStartSeconds) {
                earliestStartSeconds = startSeconds;
                found = true;
            }
        }
    }

    if (!found) {
        return;
    }

    const int64_t sourceFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(earliestStartSeconds * kTimelineFps)));
    const int64_t timelineFrame = qMax<int64_t>(
        clip->startFrame, clip->startFrame + (sourceFrame - clip->sourceInFrame));
    m_deps.seekToTimelineFrame(timelineFrame);
}

void SpeakersTab::onSpeakerSetReference1Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!saveSpeakerTrackingReference(speakerId, 1)) {
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

void SpeakersTab::onSpeakerSetReference2Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!saveSpeakerTrackingReference(speakerId, 2)) {
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

void SpeakersTab::onSpeakerPickReference1Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    m_pendingReferencePick = (m_pendingReferencePick == 1) ? 0 : 1;
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakerPickReference2Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    m_pendingReferencePick = (m_pendingReferencePick == 2) ? 0 : 2;
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakerTrackingChipClicked()
{
    if (!activeCutMutable()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    if (!transcriptTrackingHasPointstream(tracking)) {
        if (m_widgets.speakerTrackingStatusLabel) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Cannot enable Tracking: no pointstream exists yet. Generate BoxStream first."));
        }
        return;
    }
    const bool currentlyEnabled = transcriptTrackingEnabled(tracking);
    if (!setSpeakerTrackingEnabled(speakerId, !currentlyEnabled)) {
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

void SpeakersTab::onSpeakerStabilizeChipClicked()
{
    if (!activeCutMutable() || !m_speakerDeps.updateClipById) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    const bool hasFaceStabilizeKeys = !selectedClip->speakerFramingKeyframes.isEmpty();
    const bool hasRuntimeBinding = !selectedClip->speakerFramingSpeakerId.trimmed().isEmpty();
    const bool hasFramingData = hasFaceStabilizeKeys || hasRuntimeBinding;
    const bool requestedEnabled = !selectedClip->speakerFramingEnabled;
    if (requestedEnabled && !hasFramingData) {
        if (m_widgets.speakerTrackingStatusLabel) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Cannot enable Face Stabilize: no runtime BoxStream binding. Generate BoxStream first."));
        }
        return;
    }
    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        editableClip.speakerFramingEnabled = requestedEnabled;
    });
    if (!changed) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    if (m_speakerDeps.refreshPreview) {
        m_speakerDeps.refreshPreview();
    }
    refresh();
}

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
