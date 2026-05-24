#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "editor_tab_edit_effects.h"
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

bool resolvePlayheadTrackSelection(const TimelineClip& clip,
                                   const QJsonObject& streamObj,
                                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                                   int64_t playheadTimelineFrame,
                                   int64_t playheadSourceFrame,
                                   FacestreamResolvedSelection* selectionOut)
{
    if (!selectionOut) {
        return false;
    }

    const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
    const QString streamId =
        streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
    const QString trackSource =
        streamObj.value(QStringLiteral("source")).toString().trimmed().toLower();
    if (trackId < 0) {
        return false;
    }

    const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
    if (keyframes.isEmpty()) {
        return false;
    }

    FacestreamResolvedTrack track;
    track.trackId = trackId;
    track.streamId = streamId;
    track.source = trackSource;
    track.keyframes.reserve(keyframes.size());
    int64_t minStreamFrame = std::numeric_limits<int64_t>::max();
    int64_t maxStreamFrame = std::numeric_limits<int64_t>::min();
    for (const QJsonValue& keyframeValue : keyframes) {
        const QJsonObject keyframeObj = keyframeValue.toObject();
        if (!keyframeObj.contains(QStringLiteral("frame"))) {
            continue;
        }
        FacestreamResolvedKeyframe keyframe;
        keyframe.frame =
            keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        keyframe.xNorm = qBound<qreal>(
            0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        keyframe.yNorm = qBound<qreal>(
            0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
        keyframe.boxSizeNorm = qBound<qreal>(
            0.01,
            keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(
                keyframeObj.value(QStringLiteral("box")).toDouble(0.2)),
            1.0);
        keyframe.confidence = qBound<qreal>(
            0.0, keyframeObj.value(QStringLiteral("confidence")).toDouble(1.0), 1.0);
        keyframe.source =
            keyframeObj.value(QStringLiteral("source")).toString(trackSource).trimmed().toLower();
        keyframe.hasCenterBox = true;
        track.keyframes.push_back(keyframe);
        minStreamFrame = qMin(minStreamFrame, keyframe.frame);
        maxStreamFrame = qMax(maxStreamFrame, keyframe.frame);
    }
    if (track.keyframes.isEmpty()) {
        return false;
    }

    std::sort(track.keyframes.begin(), track.keyframes.end(), [](const FacestreamResolvedKeyframe& a, const FacestreamResolvedKeyframe& b) {
        return a.frame < b.frame;
    });

    QVector<int64_t> sortedFrames;
    sortedFrames.reserve(track.keyframes.size());
    for (const FacestreamResolvedKeyframe& keyframe : track.keyframes) {
        sortedFrames.push_back(keyframe.frame);
    }
    track.typicalFrameStep = qMax<int64_t>(1, facedetectionsTypicalFrameStep(sortedFrames));

    if (!parseFacestreamFrameDomainString(
            streamObj.value(QStringLiteral("frame_domain")).toString().trimmed(),
            &track.frameDomain)) {
        track.frameDomain = inferFacestreamFrameDomain(clip, minStreamFrame, maxStreamFrame);
    }
    return resolveFacestreamTrackAtPlayhead(
        clip,
        track,
        renderSyncMarkers,
        playheadTimelineFrame,
        playheadSourceFrame,
        selectionOut);
}
} // namespace

void SpeakersTab::refreshPlayheadTrackCandidatesList(const TimelineClip& clip, const QString& speakerId)
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    auto finalizeRefreshTiming = [this, &refreshTimer]() {
        m_lastPlayheadTrackCandidatesRefreshDurationMs = refreshTimer.elapsed();
        m_maxPlayheadTrackCandidatesRefreshDurationMs =
            qMax(m_maxPlayheadTrackCandidatesRefreshDurationMs,
                 m_lastPlayheadTrackCandidatesRefreshDurationMs);
    };
    if (!m_widgets.speakerPlayheadFaceDetectionsList) {
        finalizeRefreshTiming();
        return;
    }

    m_widgets.speakerPlayheadFaceDetectionsList->clear();
    m_lastPlayheadTrackCandidateCount = 0;
    const QJsonArray streams = continuityStreamsForClip(clip);
    if (streams.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("No Continuity Tracks"));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setSizeHint(QSize(140, 40));
        m_widgets.speakerPlayheadFaceDetectionsList->addItem(item);
        finalizeRefreshTiming();
        return;
    }

    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};
    const int64_t playheadTimelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip.startFrame;
    const int64_t playheadSourceFrame =
        sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(playheadTimelineFrame), renderSyncMarkers);
    const QHash<int, QString> assignedIdentityByTrackId = resolvedIdentityByTrackId(clip, streams);
    const qreal sourceFps = resolvedSourceFps(clip);
    for (const QJsonValue& value : streams) {
        const QJsonObject streamObj = value.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        FacestreamResolvedSelection selection;
        if (!resolvePlayheadTrackSelection(clip,
                                           streamObj,
                                           renderSyncMarkers,
                                           playheadTimelineFrame,
                                           playheadSourceFrame,
                                           &selection)) {
            continue;
        }

        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
        const QString assignedSpeakerId = assignedIdentityByTrackId.value(trackId).trimmed();
        const QString assignedLabel =
            assignedSpeakerId.isEmpty() ? QStringLiteral("Unassigned") : speakerDisplayLabel(assignedSpeakerId);
        QListWidgetItem* item = new QListWidgetItem(
            QIcon(faceStreamPreviewAvatarWithDecoder(
                clip,
                speakerId,
                QJsonObject{
                    {QString(kTranscriptSpeakerTrackingFrameKey),
                     static_cast<qint64>(qMax<int64_t>(0, selection.sourceFrame))},
                    {QString(kTranscriptSpeakerLocationXKey), selection.keyframe.xNorm},
                    {QString(kTranscriptSpeakerLocationYKey), selection.keyframe.yNorm},
                    {QString(kTranscriptSpeakerTrackingBoxSizeKey), selection.keyframe.boxSizeNorm},
                    {QStringLiteral("confidence"), selection.keyframe.confidence},
                    {QStringLiteral("source"), selection.keyframe.source},
                },
                72,
                nullptr,
                nullptr,
                sourceFps)),
            QStringLiteral("%1\n%2").arg(streamId, assignedLabel));
        item->setData(Qt::UserRole, trackId);
        item->setData(Qt::UserRole + 1, streamId);
        item->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(selection.sourceFrame));
        item->setData(
            Qt::UserRole + 3,
            qBound<qreal>(
                0.0,
                selection.keyframe.xNorm,
                1.0));
        item->setData(
            Qt::UserRole + 4,
            qBound<qreal>(
                0.0,
                selection.keyframe.yNorm,
                1.0));
        item->setData(
            Qt::UserRole + 5,
            qBound<qreal>(
                0.01,
                selection.keyframe.boxSizeNorm,
                1.0));
        item->setToolTip(
            QStringLiteral("Track %1 | Frame %2 | Source %3 | Current assignment: %4")
                .arg(trackId)
                .arg(selection.sourceFrame)
                .arg(selection.keyframe.source.isEmpty() ? QStringLiteral("-") : selection.keyframe.source)
                .arg(assignedLabel));
        item->setSizeHint(QSize(100, 96));
        if (assignedSpeakerId == speakerId) {
            item->setSelected(true);
        }
        m_widgets.speakerPlayheadFaceDetectionsList->addItem(item);
        ++m_lastPlayheadTrackCandidateCount;
    }

    if (m_widgets.speakerPlayheadFaceDetectionsList->count() == 0) {
        auto* item = new QListWidgetItem(QStringLiteral("No Tracks At Playhead"));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setSizeHint(QSize(150, 40));
        m_widgets.speakerPlayheadFaceDetectionsList->addItem(item);
    }
    finalizeRefreshTiming();
}

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
        !m_widgets.speakerPlayheadFaceDetectionsList &&
        !m_widgets.selectedSpeakerRef1ImageLabel &&
        !m_widgets.selectedSpeakerRef2ImageLabel) {
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
        if (m_widgets.selectedSpeakerRef1ImageLabel) {
            m_widgets.selectedSpeakerRef1ImageLabel->clear();
            m_widgets.selectedSpeakerRef1ImageLabel->hide();
        }
        if (m_widgets.selectedSpeakerRef2ImageLabel) {
            m_widgets.selectedSpeakerRef2ImageLabel->clear();
            m_widgets.selectedSpeakerRef2ImageLabel->hide();
        }
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
        if (m_widgets.selectedSpeakerRef1ImageLabel) {
            m_widgets.selectedSpeakerRef1ImageLabel->clear();
            m_widgets.selectedSpeakerRef1ImageLabel->hide();
        }
        if (m_widgets.selectedSpeakerRef2ImageLabel) {
            m_widgets.selectedSpeakerRef2ImageLabel->clear();
            m_widgets.selectedSpeakerRef2ImageLabel->hide();
        }
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
        const QJsonArray faceRefs = speakerFaceRefs(profile);
        for (const QJsonValue& faceRefValue : faceRefs) {
            const QJsonObject faceRef = faceRefValue.toObject();
            const QJsonObject keyframeObj = previewKeyframeFromSpeakerFaceRef(faceRef);
            if (keyframeObj.isEmpty()) {
                continue;
            }
            const int trackId = faceRef.value(QStringLiteral("track_id")).toInt(-1);
            const QString streamId =
                faceRef.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
            QListWidgetItem* item = new QListWidgetItem(
                QIcon(faceStreamPreviewAvatar(*clip, speakerId, keyframeObj, 72)),
                streamId);
            item->setToolTip(QStringLiteral("Track %1 | Frame %2")
                                 .arg(trackId)
                                 .arg(faceRef.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong()));
            item->setData(Qt::UserRole, trackId);
            item->setSizeHint(QSize(92, 96));
            m_widgets.selectedSpeakerFaceDetectionsList->addItem(item);
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
    if (m_widgets.selectedSpeakerRef1ImageLabel) {
        m_widgets.selectedSpeakerRef1ImageLabel->clear();
        m_widgets.selectedSpeakerRef1ImageLabel->hide();
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->clear();
        m_widgets.selectedSpeakerRef2ImageLabel->hide();
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

bool SpeakersTab::saveSpeakerTrackingReferenceAt(const QString& speakerId,
                                                 int referenceIndex,
                                                 int64_t frame,
                                                 qreal xNorm,
                                                 qreal yNorm,
                                                 qreal boxSizeNorm)
{
    if (!m_transcriptSession.hasObjectDocument() || speakerId.isEmpty()) {
        return false;
    }

    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::saveTrackingReferenceAt(
            m_transcriptSession, speakerId, referenceIndex, frame, xNorm, yNorm, boxSizeNorm);
    if (!result.ok || !result.changed) {
        return false;
    }
    return saveLoadedTranscriptDocument();
}

bool SpeakersTab::saveSpeakerTrackingReference(const QString& speakerId, int referenceIndex)
{
    if (!m_transcriptSession.hasObjectDocument() || speakerId.isEmpty()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t frame = currentSourceFrameForClip(*clip);

    QJsonObject root = m_transcriptSession.rootObject();
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
    if (!m_transcriptSession.hasObjectDocument() || speakerId.isEmpty()) {
        return false;
    }

    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::clearTrackingReferences(m_transcriptSession, speakerId);
    if (!result.ok) {
        return false;
    }
    if (!result.changed) {
        return true;
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

bool SpeakersTab::selectedSpeakerReferenceObject(int referenceIndex,
                                                 QString* speakerIdOut,
                                                 QJsonObject* refOut) const
{
    if (referenceIndex != 1 && referenceIndex != 2) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    const QJsonObject profiles =
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
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
    applyTabEditEffects(speakerEditCallbacks(m_deps),
                        TabEditEffects{.refreshInspector = false});
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
    const QJsonObject tracking = speakerFramingObject(profile);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);
    const bool hasTrackingModel = transcriptTrackingHasPointstream(tracking);
    const bool canMutate = activeCutMutable();

    QAction* enableTrackingAction = menu.addAction(QStringLiteral("Enable Speaker Tracking"));
    QAction* disableTrackingAction = menu.addAction(QStringLiteral("Disable Speaker Tracking"));
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
    enableTrackingAction->setEnabled(canMutate && hasTrackingModel && !trackingEnabled);
    disableTrackingAction->setEnabled(canMutate && trackingEnabled);
    if (runAutoTrackAction) {
        runAutoTrackAction->setEnabled(canMutate);
    }
    if (viewAutoTrackAction) {
        viewAutoTrackAction->setEnabled(true);
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
        if (chosen != enableTrackingAction &&
            chosen != disableTrackingAction &&
            chosen != runAutoTrackAction &&
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

    if (chosen == enableTrackingAction || chosen == disableTrackingAction) {
        const bool enable = (chosen == enableTrackingAction);
        if (!setSpeakerTrackingEnabled(speakerId, enable)) {
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
    applySpeakerDocumentEffects(m_deps);
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
    applySpeakerDocumentEffects(m_deps);
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
    const bool requestedEnabled = !selectedClip->speakerFramingEnabled;
    if (requestedEnabled && !hasFramingData) {
        if (m_widgets.speakerTrackingStatusLabel) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Cannot enable Face Stabilize: no runtime FaceDetections binding. Generate FaceDetections first."));
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
    applyTabEditEffects(
        TabEditCallbacks{
            .updatePreview = m_speakerDeps.refreshPreview,
            .scheduleSave = m_deps.scheduleSaveState,
            .pushHistory = m_deps.pushHistorySnapshot,
        },
        TabEditEffects{.refreshInspector = false});
    refresh();
}
