#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "editor_tab_edit_effects.h"
#include "editor_shared_keyframes.h"
#include "keyframe_table_shared.h"
#include "speaker_document_edit_ops.h"

#include "decoder_context.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_time_mapping.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QGuiApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {
TabEditCallbacks speakerEditCallbacks(const TableTabBase::Dependencies& deps) {
    return TabEditCallbacks{
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

void applySpeakerDocumentEffects(const TableTabBase::Dependencies& deps) {
    applyTabEditEffects(speakerEditCallbacks(deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

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

void SpeakersTab::updatePlayheadTrackCandidatesVisibility()
{
    const bool showPlayheadTracks =
        !m_widgets.speakerShowPlayheadFaceDetectionsCheckBox ||
        m_widgets.speakerShowPlayheadFaceDetectionsCheckBox->isChecked();
    if (m_widgets.speakerPlayheadFaceDetectionsList) {
        m_widgets.speakerPlayheadFaceDetectionsList->setVisible(showPlayheadTracks);
    }
    if (m_widgets.speakerPrecropFacesButton) {
        const bool hasSpeaker = !selectedSpeakerId().trimmed().isEmpty();
        const bool hasSelection =
            m_widgets.speakerPlayheadFaceDetectionsList &&
            !m_widgets.speakerPlayheadFaceDetectionsList->selectedItems().isEmpty();
        m_widgets.speakerPrecropFacesButton->setEnabled(
            activeCutMutable() && showPlayheadTracks && hasSpeaker && hasSelection);
    }
}

void SpeakersTab::updateSelectedSpeakerPanel()
{
    if (!m_widgets.selectedSpeakerIdLabel &&
        !m_widgets.selectedSpeakerFaceDetectionsList &&
        !m_widgets.speakerPlayheadFaceDetectionsList) {
        return;
    }

    const QString speakerId = selectedSpeakerId();
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !m_transcriptSession.hasObjectDocument()) {
        if (m_widgets.selectedSpeakerIdLabel) {
            m_widgets.selectedSpeakerIdLabel->setText(
                speakerId.isEmpty() ? QStringLiteral("No speaker selected") : speakerDisplayLabel(speakerId));
            m_widgets.selectedSpeakerIdLabel->setToolTip(QString());
        }
        if (m_widgets.speakerCurrentSentenceLabel) {
            m_widgets.speakerCurrentSentenceLabel->setText(
                QStringLiteral("Select a speaker to assign tracks and view sentence context."));
        }
        if (m_widgets.selectedSpeakerFaceDetectionsList) {
            m_widgets.selectedSpeakerFaceDetectionsList->clear();
        }
        if (m_widgets.speakerPlayheadFaceDetectionsList) {
            m_widgets.speakerPlayheadFaceDetectionsList->clear();
        }
        updatePlayheadTrackCandidatesVisibility();
        updateSpeakerFramingTargetControls();
        return;
    }

    if (speakerId.isEmpty()) {
        if (m_widgets.selectedSpeakerIdLabel) {
            m_widgets.selectedSpeakerIdLabel->setText(QStringLiteral("No speaker selected"));
            m_widgets.selectedSpeakerIdLabel->setToolTip(QString());
        }
        if (m_widgets.speakerCurrentSentenceLabel) {
            m_widgets.speakerCurrentSentenceLabel->setText(
                QStringLiteral("Select a speaker to assign tracks and view sentence context."));
        }
        if (m_widgets.selectedSpeakerFaceDetectionsList) {
            m_widgets.selectedSpeakerFaceDetectionsList->clear();
            auto* item = new QListWidgetItem(QStringLiteral("Select Speaker To View Assignments"));
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            item->setSizeHint(QSize(180, 40));
            m_widgets.selectedSpeakerFaceDetectionsList->addItem(item);
        }
        refreshPlayheadTrackCandidatesList(*clip, QString());
        updatePlayheadTrackCandidatesVisibility();
        updateSpeakerFramingTargetControls();
        return;
    }

    const QJsonObject root = m_transcriptSession.rootObject();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QString displayName = speakerDisplayLabel(speakerId);
    if (m_widgets.selectedSpeakerIdLabel) {
        m_widgets.selectedSpeakerIdLabel->setText(displayName);
        if (displayName != speakerId) {
            m_widgets.selectedSpeakerIdLabel->setToolTip(QStringLiteral("Speaker ID: %1").arg(speakerId));
        } else {
            m_widgets.selectedSpeakerIdLabel->setToolTip(QString());
        }
    }
    if (m_widgets.speakerCurrentSentenceLabel) {
        m_widgets.speakerCurrentSentenceLabel->setText(
            currentSpeakerSentenceAtCurrentFrame(speakerId));
    }

    if (m_widgets.selectedSpeakerFaceDetectionsList) {
        m_widgets.selectedSpeakerFaceDetectionsList->clear();
        const QJsonArray streams = continuityStreamsForClip(*clip);
        const QVector<int> assignedTrackIds =
            resolvedAssignedTrackIdsForSpeaker(*clip, streams, speakerId);
        std::unique_ptr<editor::DecoderContext> decoder;
        const QString mediaPath = interactivePreviewMediaPathForClip(*clip);
        if (!mediaPath.isEmpty()) {
            decoder = std::make_unique<editor::DecoderContext>(mediaPath);
            if (!decoder->initialize()) {
                decoder.reset();
            }
        }
        QHash<int64_t, QImage> frameImageCache;
        for (int assignedTrackId : assignedTrackIds) {
            for (const QJsonValue& streamValue : streams) {
                const QJsonObject streamObj = streamValue.toObject();
                const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
                if (trackId != assignedTrackId) {
                    continue;
                }
                const QString streamId =
                    streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
                const QJsonObject keyframeObj = representativeKeyframeForTrack(*clip, streamObj);
                QListWidgetItem* item = new QListWidgetItem(
                    QIcon(continuityTrackAvatar(
                        *clip,
                        speakerId,
                        streamObj,
                        72,
                        decoder.get(),
                        &frameImageCache)),
                    streamId);
                item->setToolTip(QStringLiteral("Track %1 | Frame %2")
                                     .arg(trackId)
                                     .arg(keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong()));
                item->setData(Qt::UserRole, trackId);
                item->setSizeHint(QSize(92, 96));
                m_widgets.selectedSpeakerFaceDetectionsList->addItem(item);
                break;
            }
        }
        if (m_widgets.selectedSpeakerFaceDetectionsList->count() == 0) {
            auto* item = new QListWidgetItem(QStringLiteral("No FaceDetections"));
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            item->setSizeHint(QSize(110, 40));
            m_widgets.selectedSpeakerFaceDetectionsList->addItem(item);
        }
    }
    refreshPlayheadTrackCandidatesList(*clip, speakerId);
    updatePlayheadTrackCandidatesVisibility();
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
    const int64_t timelineFrame =
        clip && m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const TimelineClip::TransformKeyframe targetState =
        clip ? evaluateClipSpeakerFramingTargetAtFrame(*clip, timelineFrame)
             : TimelineClip::TransformKeyframe{};
    const bool framingEnabledAtFrame =
        clip ? evaluateClipSpeakerFramingEnabledAtFrame(*clip, timelineFrame) : false;
    m_updatingSpeakerFramingTargetControls = true;
    if (m_widgets.speakerFramingTargetXSpin) {
        QSignalBlocker blocker(m_widgets.speakerFramingTargetXSpin);
        m_widgets.speakerFramingTargetXSpin->setValue(
            clip ? qBound<qreal>(0.0, targetState.translationX, 1.0) : 0.5);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        QSignalBlocker blocker(m_widgets.speakerFramingTargetYSpin);
        m_widgets.speakerFramingTargetYSpin->setValue(
            clip ? qBound<qreal>(0.0, targetState.translationY, 1.0) : 0.35);
    }
    const qreal boxValue = clip
        ? qBound<qreal>(-1.0, targetState.scaleX, 1.0)
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
        m_widgets.speakerApplyFramingToClipCheckBox->setChecked(framingEnabledAtFrame);
    }
    if (m_widgets.speakerFramingEnabledKeyframeTable) {
        QSignalBlocker tableBlocker(m_widgets.speakerFramingEnabledKeyframeTable);
        populateSpeakerFramingEnabledKeyframeTable(clip ? *clip : TimelineClip{});
    }
    if (m_widgets.speakerClipFramingStatusLabel) {
        const int keyCount = clip ? clip->speakerFramingKeyframes.size() : 0;
        const int targetKeyCount = clip ? clip->speakerFramingTargetKeyframes.size() : 0;
        const int enabledKeyCount = clip ? clip->speakerFramingEnabledKeyframes.size() : 0;
        m_widgets.speakerClipFramingStatusLabel->setText(
            QStringLiteral("Face Stabilize: %1 | Enable: %2 keys | Transform: %3 keys | Target: %4 keys")
                .arg(framingEnabledAtFrame ? QStringLiteral("ON") : QStringLiteral("OFF"))
                .arg(enabledKeyCount)
                .arg(keyCount)
                .arg(targetKeyCount));
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
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : selectedClip->startFrame;
    const TimelineClip::TransformKeyframe currentTarget =
        evaluateClipSpeakerFramingTargetAtFrame(*selectedClip, timelineFrame);
    const qreal targetX = m_widgets.speakerFramingTargetXSpin
        ? qBound<qreal>(0.0, m_widgets.speakerFramingTargetXSpin->value(), 1.0)
        : qBound<qreal>(0.0, currentTarget.translationX, 1.0);
    const qreal targetY = m_widgets.speakerFramingTargetYSpin
        ? qBound<qreal>(0.0, m_widgets.speakerFramingTargetYSpin->value(), 1.0)
        : qBound<qreal>(0.0, currentTarget.translationY, 1.0);
    const qreal targetBox = zoomEnabled && m_widgets.speakerFramingTargetBoxSpin
        ? qBound<qreal>(0.01, m_widgets.speakerFramingTargetBoxSpin->value(), 1.0)
        : -1.0;
    const int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - selectedClip->startFrame,
        qMax<int64_t>(0, selectedClip->durationFrames - 1));

    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        TimelineClip::TransformKeyframe keyframe;
        keyframe.frame = localFrame;
        keyframe.translationX = targetX;
        keyframe.translationY = targetY;
        keyframe.rotation = 0.0;
        keyframe.scaleX = targetBox;
        keyframe.scaleY = targetBox;
        keyframe.linearInterpolation = true;
        editableClip.speakerFramingTargetKeyframes.push_back(keyframe);
        normalizeClipTransformKeyframes(editableClip);
    });
    if (changed && m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    return changed;
}

void SpeakersTab::populateSpeakerFramingEnabledKeyframeTable(const TimelineClip& clip)
{
    if (!m_widgets.speakerFramingEnabledKeyframeTable) {
        return;
    }
    m_updatingSpeakerFramingEnabledTable = true;
    QTableWidget* table = m_widgets.speakerFramingEnabledKeyframeTable;
    table->clearContents();
    table->setRowCount(0);
    if (clip.id.trimmed().isEmpty() || !m_deps.getSelectedClip) {
        m_selectedSpeakerFramingEnabledFrame = -1;
        m_selectedSpeakerFramingEnabledFrames.clear();
        m_updatingSpeakerFramingEnabledTable = false;
        return;
    }

    table->setRowCount(clip.speakerFramingEnabledKeyframes.size());
    for (int row = 0; row < clip.speakerFramingEnabledKeyframes.size(); ++row) {
        const TimelineClip::BoolKeyframe& keyframe = clip.speakerFramingEnabledKeyframes.at(row);
        auto* frameItem = new QTableWidgetItem(QString::number(keyframe.frame));
        frameItem->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(keyframe.frame));
        auto* enabledItem = new QTableWidgetItem(keyframe.enabled ? QStringLiteral("ON") : QStringLiteral("OFF"));
        enabledItem->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(keyframe.frame));
        table->setItem(row, 0, frameItem);
        table->setItem(row, 1, enabledItem);
    }
    editor::restoreSelectionByFrameRole(table, m_selectedSpeakerFramingEnabledFrames);
    m_updatingSpeakerFramingEnabledTable = false;
}

bool SpeakersTab::upsertSpeakerFramingEnabledKeyframeAtPlayhead(bool enabled)
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip || !m_speakerDeps.updateClipById) {
        return false;
    }
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : selectedClip->startFrame;
    const int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - selectedClip->startFrame,
        qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        bool replaced = false;
        for (TimelineClip::BoolKeyframe& keyframe : editableClip.speakerFramingEnabledKeyframes) {
            if (keyframe.frame == localFrame) {
                keyframe.enabled = enabled;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            TimelineClip::BoolKeyframe keyframe;
            keyframe.frame = localFrame;
            keyframe.enabled = enabled;
            editableClip.speakerFramingEnabledKeyframes.push_back(keyframe);
        }
        if (localFrame == 0 || editableClip.speakerFramingEnabledKeyframes.size() == 1) {
            editableClip.speakerFramingEnabled = enabled;
        }
        normalizeClipTransformKeyframes(editableClip);
    });
    if (changed) {
        m_selectedSpeakerFramingEnabledFrame = localFrame;
        m_selectedSpeakerFramingEnabledFrames = {localFrame};
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
        if (m_speakerDeps.refreshPreview) {
            m_speakerDeps.refreshPreview();
        }
        updateSpeakerFramingTargetControls();
        updateSpeakerTrackingStatusLabel();
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
    return upsertSpeakerFramingEnabledKeyframeAtPlayhead(requestedEnabled);
}

bool SpeakersTab::removeSelectedSpeakerFramingEnabledKeyframes()
{
    if (!m_widgets.speakerFramingEnabledKeyframeTable || !m_speakerDeps.updateClipById) {
        return false;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        return false;
    }
    const QSet<int64_t> frames = editor::collectSelectedFrameRoles(m_widgets.speakerFramingEnabledKeyframeTable);
    if (frames.isEmpty()) {
        return false;
    }
    const bool fallbackEnabled =
        evaluateClipSpeakerFramingEnabledAtFrame(*selectedClip, m_deps.getCurrentTimelineFrame());
    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        const int before = editableClip.speakerFramingEnabledKeyframes.size();
        editableClip.speakerFramingEnabledKeyframes.erase(
            std::remove_if(editableClip.speakerFramingEnabledKeyframes.begin(),
                           editableClip.speakerFramingEnabledKeyframes.end(),
                           [&frames](const TimelineClip::BoolKeyframe& keyframe) {
                               return frames.contains(keyframe.frame);
                           }),
            editableClip.speakerFramingEnabledKeyframes.end());
        if (editableClip.speakerFramingEnabledKeyframes.size() == before) {
            return;
        }
        if (editableClip.speakerFramingEnabledKeyframes.isEmpty()) {
            editableClip.speakerFramingEnabled = fallbackEnabled;
        }
        normalizeClipTransformKeyframes(editableClip);
    });
    if (changed) {
        m_selectedSpeakerFramingEnabledFrame = -1;
        m_selectedSpeakerFramingEnabledFrames.clear();
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
        if (m_speakerDeps.refreshPreview) {
            m_speakerDeps.refreshPreview();
        }
        updateSpeakerFramingTargetControls();
        updateSpeakerTrackingStatusLabel();
    }
    return changed;
}

void SpeakersTab::onSpeakerFramingEnabledTableSelectionChanged()
{
    if (m_updatingSpeakerFramingEnabledTable || !m_widgets.speakerFramingEnabledKeyframeTable) {
        return;
    }
    const QSet<int64_t> frames = editor::collectSelectedFrameRoles(m_widgets.speakerFramingEnabledKeyframeTable);
    const int64_t frame = editor::primarySelectedFrameRole(m_widgets.speakerFramingEnabledKeyframeTable);
    m_selectedSpeakerFramingEnabledFrames = frames;
    m_selectedSpeakerFramingEnabledFrame = frame;
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (clip && frame >= 0 && m_deps.seekToTimelineFrame) {
        requestDeferredSeek(clip->startFrame + frame);
    }
}

void SpeakersTab::onSpeakerFramingEnabledTableItemChanged(QTableWidgetItem* item)
{
    if (m_updatingSpeakerFramingEnabledTable || !item || !m_speakerDeps.updateClipById) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        return;
    }
    const int row = item->row();
    QTableWidgetItem* frameItem = m_widgets.speakerFramingEnabledKeyframeTable->item(row, 0);
    QTableWidgetItem* enabledItem = m_widgets.speakerFramingEnabledKeyframeTable->item(row, 1);
    if (!frameItem || !enabledItem) {
        return;
    }
    const int64_t oldFrame = frameItem->data(Qt::UserRole).toLongLong();
    const int64_t newFrame = qBound<int64_t>(
        0,
        static_cast<int64_t>(frameItem->text().trimmed().toLongLong()),
        qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const QString enabledText = enabledItem->text().trimmed().toLower();
    const bool enabled =
        enabledText == QStringLiteral("on") ||
        enabledText == QStringLiteral("true") ||
        enabledText == QStringLiteral("1") ||
        enabledText == QStringLiteral("yes");
    const bool changed = m_speakerDeps.updateClipById(selectedClip->id, [&](TimelineClip& editableClip) {
        for (TimelineClip::BoolKeyframe& keyframe : editableClip.speakerFramingEnabledKeyframes) {
            if (keyframe.frame == oldFrame) {
                keyframe.frame = newFrame;
                keyframe.enabled = enabled;
                break;
            }
        }
        normalizeClipTransformKeyframes(editableClip);
    });
    if (changed) {
        m_selectedSpeakerFramingEnabledFrame = newFrame;
        m_selectedSpeakerFramingEnabledFrames = {newFrame};
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
        if (m_speakerDeps.refreshPreview) {
            m_speakerDeps.refreshPreview();
        }
        updateSpeakerFramingTargetControls();
    }
}

void SpeakersTab::onSpeakerFramingEnabledTableContextMenu(const QPoint& pos)
{
    if (!m_widgets.speakerFramingEnabledKeyframeTable) {
        return;
    }
    int row = -1;
    editor::ensureContextRowSelected(m_widgets.speakerFramingEnabledKeyframeTable, pos, &row);
    QMenu menu(m_widgets.speakerFramingEnabledKeyframeTable);
    QAction* addOn = menu.addAction(QStringLiteral("Add ON Keyframe At Playhead"));
    QAction* addOff = menu.addAction(QStringLiteral("Add OFF Keyframe At Playhead"));
    menu.addSeparator();
    QAction* deleteRows = menu.addAction(QStringLiteral("Delete Selected Keyframes"));
    deleteRows->setEnabled(!editor::collectSelectedFrameRoles(m_widgets.speakerFramingEnabledKeyframeTable).isEmpty());
    QAction* chosen = menu.exec(m_widgets.speakerFramingEnabledKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == addOn) {
        upsertSpeakerFramingEnabledKeyframeAtPlayhead(true);
    } else if (chosen == addOff) {
        upsertSpeakerFramingEnabledKeyframeAtPlayhead(false);
    } else if (chosen == deleteRows) {
        removeSelectedSpeakerFramingEnabledKeyframes();
    }
}

void SpeakersTab::syncSpeakerFramingEnabledTableToPlayhead()
{
    if (!m_widgets.speakerFramingEnabledKeyframeTable ||
        shouldSkipSyncToPlayhead(m_widgets.speakerFramingEnabledKeyframeTable, nullptr)) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->speakerFramingEnabledKeyframes.isEmpty()) {
        return;
    }
    const int64_t localFrame = qBound<int64_t>(
        0,
        m_deps.getCurrentTimelineFrame() - clip->startFrame,
        qMax<int64_t>(0, clip->durationFrames - 1));
    int row = 0;
    for (int i = 0; i < m_widgets.speakerFramingEnabledKeyframeTable->rowCount(); ++i) {
        const int64_t frame = editor::rowFrameRole(m_widgets.speakerFramingEnabledKeyframeTable, i);
        if (frame <= localFrame) {
            row = i;
        }
    }
    m_updatingSpeakerFramingEnabledTable = true;
    m_widgets.speakerFramingEnabledKeyframeTable->selectRow(row);
    m_updatingSpeakerFramingEnabledTable = false;
}

QString SpeakersTab::currentSpeakerSentenceAtCurrentFrame(const QString& speakerId) const
{
    if (speakerId.trimmed().isEmpty()) {
        return QStringLiteral("No sentence available.");
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return QStringLiteral("No sentence available.");
    }
    const int64_t sourceFrame = currentSourceFrameForClip(*clip);
    if (m_transcriptSession.transcriptPath().trimmed().isEmpty()) {
        return QStringLiteral("No sentence available.");
    }
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(m_transcriptSession.transcriptPath());
    if (!runtimeDocument) {
        return QStringLiteral("No sentence found for this speaker.");
    }
    return transcriptSpeakerSentenceForSourceFrame(*runtimeDocument, speakerId, sourceFrame);
}

void SpeakersTab::syncCurrentSpeakerSentenceToPlayhead()
{
    QLabel* const sentenceLabel = m_widgets.speakerCurrentSentenceLabel;
    if (m_updating) {
        return;
    }

    const QString speakerId = selectedSpeakerId();
    if (sentenceLabel) {
        const QString nextText = speakerId.isEmpty()
            ? QStringLiteral("Select a speaker to view sentence context.")
            : currentSpeakerSentenceAtCurrentFrame(speakerId);
        if (sentenceLabel->text() != nextText) {
            sentenceLabel->setText(nextText);
        }
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (clip && !speakerId.isEmpty() && m_transcriptSession.hasObjectDocument()) {
        refreshPlayheadTrackCandidatesList(*clip, speakerId);
    }
    updateSpeakerFramingTargetControls();
    syncSpeakerFramingEnabledTableToPlayhead();
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
    if (!m_widgets.speakersTable || !m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(tableRow, 1);
    if (!speakerItem) {
        return false;
    }
    const QString speakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
    if (speakerId.isEmpty()) {
        return false;
    }

    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::applyProfileCellEdit(m_transcriptSession, speakerId, column, valueText);
    if (!result.ok || !result.changed) {
        return false;
    }
    return saveLoadedTranscriptDocument();
}

bool SpeakersTab::setSpeakerTrackingEnabled(const QString& speakerId, bool enabled)
{
    if (!m_transcriptSession.hasObjectDocument() || speakerId.isEmpty()) {
        return false;
    }

    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::setTrackingEnabled(m_transcriptSession, speakerId, enabled);
    if (!result.ok) {
        return false;
    }
    if (!result.changed) {
        return true;
    }
    return saveLoadedTranscriptDocument();
}

bool SpeakersTab::deleteSpeakerAutoTrackPointstream(const QString& speakerId)
{
    if (!m_transcriptSession.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::deleteAutoTrackPointstream(m_transcriptSession, speakerId);
    if (!result.ok) {
        return false;
    }
    if (!result.changed) {
        return true;
    }
    return saveLoadedTranscriptDocument();
}

bool SpeakersTab::setSpeakerSkipped(const QString& speakerId, bool skipped)
{
    if (!m_transcriptSession.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::setSpeakerSkipped(m_transcriptSession, speakerId, skipped);
    if (!result.ok) {
        return !result.changed;
    }
    if (!result.changed) {
        return true;
    }
    return saveLoadedTranscriptDocument();
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
    if (!m_widgets.speakersTable || !m_transcriptSession.hasObjectDocument()) {
        return;
    }

    const int row = m_widgets.speakersTable->itemAt(pos)
        ? m_widgets.speakersTable->itemAt(pos)->row()
        : m_widgets.speakersTable->currentRow();
    if (row < 0) {
        return;
    }

    QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(row, 1);
    if (!speakerItem) {
        return;
    }
    const QString speakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
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
    const QJsonArray segments = m_transcriptSession.rootObject().value(QStringLiteral("segments")).toArray();
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
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const bool canMutate = activeCutMutable();

    QMenu* autoTrackMenu = menu.addMenu(QStringLiteral("FaceDetections"));
    QAction* runAutoTrackAction = nullptr;
    QAction* viewAutoTrackAction = nullptr;
    QAction* deleteAutoTrackAction = nullptr;
    runAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("Generate FaceDetections"));
    viewAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("View FaceDetections"));
    deleteAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("Delete FaceDetections"));
    menu.addSeparator();
    const QString exportLabel = selectedSpeakerIds.size() > 1
        ? QStringLiteral("Export Video (%1 Speakers)").arg(selectedSpeakerIds.size())
        : QStringLiteral("Export Video");
    QAction* exportVideoAction = menu.addAction(exportLabel);

    skipAction->setEnabled(canMutate && wordCount > 0 && skippedCount < wordCount);
    unskipAction->setEnabled(canMutate && wordCount > 0 && skippedCount > 0);
    if (runAutoTrackAction) {
        runAutoTrackAction->setEnabled(canMutate);
    }
    if (viewAutoTrackAction) {
        viewAutoTrackAction->setEnabled(true);
    }
    if (deleteAutoTrackAction) {
        deleteAutoTrackAction->setEnabled(canMutate &&
                                          transcriptTrackingHasPointstream(speakerFramingObject(profile)));
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
    if (chosen == viewAutoTrackAction) {
        onSpeakerViewFaceDetectionsClicked();
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
        if (chosen != runAutoTrackAction &&
            chosen != deleteAutoTrackAction) {
            return;
        }
    }

    if (chosen == runAutoTrackAction) {
        onSpeakerRunAutoTrackClicked();
        return;
    }

    if (chosen == deleteAutoTrackAction) {
        if (!deleteSpeakerAutoTrackPointstream(speakerId)) {
            refresh();
            return;
        }
        emit transcriptDocumentChanged();
        applySpeakerDocumentEffects(m_deps);
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
    applySpeakerDocumentEffects(m_deps);
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
    if (column == 4 && activeCutMutable()) {
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
        applySpeakerDocumentEffects(m_deps);
        refresh();
        return;
    }
    // Preserve in-place editing workflow for editable columns.
    if (column == 2 || column == 3 || column == 4) {
        return;
    }
    const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
    if (!speakerId.isEmpty()) {
        seekToSpeakerFirstWord(speakerId);
    }
}

void SpeakersTab::seekToSpeakerFirstWord(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_transcriptSession.hasObjectDocument() || !m_deps.seekToTimelineFrame) {
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

    const QJsonArray segments = m_transcriptSession.rootObject().value(QStringLiteral("segments")).toArray();
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
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    if (!transcriptTrackingHasPointstream(tracking)) {
        if (m_widgets.speakerTrackingStatusLabel) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Cannot enable Speaker Tracking: no pointstream exists yet. Generate FaceDetections first."));
        }
        return;
    }
    const bool currentlyEnabled = transcriptTrackingEnabled(tracking);
    if (!setSpeakerTrackingEnabled(speakerId, !currentlyEnabled)) {
        refresh();
        return;
    }
    emit transcriptDocumentChanged();
    applySpeakerDocumentEffects(m_deps);
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
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : selectedClip->startFrame;
    const bool requestedEnabled = !evaluateClipSpeakerFramingEnabledAtFrame(*selectedClip, timelineFrame);
    if (requestedEnabled && !hasFramingData) {
        if (m_widgets.speakerTrackingStatusLabel) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Cannot enable Face Stabilize: no runtime FaceDetections binding. Generate FaceDetections first."));
        }
        return;
    }
    if (!upsertSpeakerFramingEnabledKeyframeAtPlayhead(requestedEnabled)) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    refresh();
}
