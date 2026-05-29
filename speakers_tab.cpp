#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speakers_table.h"

#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "facedetections_artifact_utils.h"
#include "decoder_context.h"
#include "editor_shared_transcript.h"
#include "editor_shared_keyframes.h"
#include "speaker_flow_debug.h"
#include "transcript_engine.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QImage>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QFrame>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSet>
#include <QSignalBlocker>
#include <memory>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QStringList>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace {

bool uiAutomationEnabled()
{
    return qEnvironmentVariableIntValue("JCUT_UI_AUTOMATION") > 0;
}

struct ContinuityCoverageSummary {
    QString importedArtifactDir;
    QString runId;
    qint64 minRawTrackFrame = -1;
    qint64 maxRawTrackFrame = -1;
    int rawTrackCount = 0;
    bool hasRawTrackCoverage = false;
};

int64_t transcriptJsonFrameForSeconds(double seconds, bool roundUp)
{
    if (seconds < 0.0) {
        return -1;
    }
    const double frame = seconds * static_cast<double>(kTimelineFps);
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(roundUp ? std::ceil(frame) : std::floor(frame)));
}

QString activeSpeakerIdInTranscriptRootAtSourceFrame(const QJsonObject& transcriptRoot, int64_t sourceFrame)
{
    if (sourceFrame < 0) {
        return QString();
    }
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const int64_t segmentStartFrame =
            transcriptJsonFrameForSeconds(segmentObj.value(QStringLiteral("start")).toDouble(-1.0), false);
        const int64_t segmentEndFrame =
            transcriptJsonFrameForSeconds(segmentObj.value(QStringLiteral("end")).toDouble(-1.0), true);
        if (segmentEndFrame >= 0 && sourceFrame > segmentEndFrame) {
            continue;
        }
        if (segmentStartFrame >= 0 && sourceFrame < segmentStartFrame) {
            return QString();
        }

        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        if (words.isEmpty()) {
            return segmentSpeaker;
        }

        int bestIndex = -1;
        QStringList wordSpeakers;
        wordSpeakers.reserve(words.size());
        for (int i = 0; i < words.size(); ++i) {
            const QJsonObject wordObj = words.at(i).toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                wordSpeakers.push_back(QString());
                continue;
            }
            QString speakerId =
                wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (speakerId.isEmpty()) {
                speakerId = segmentSpeaker;
            }
            wordSpeakers.push_back(speakerId);
            const int64_t startFrame =
                transcriptJsonFrameForSeconds(wordObj.value(QStringLiteral("start")).toDouble(-1.0), false);
            const int64_t endFrame =
                transcriptJsonFrameForSeconds(wordObj.value(QStringLiteral("end")).toDouble(-1.0), true);
            if (startFrame >= 0 && endFrame >= startFrame &&
                sourceFrame >= startFrame && sourceFrame <= endFrame) {
                bestIndex = i;
                break;
            }
            if (endFrame >= 0 && sourceFrame > endFrame && !speakerId.isEmpty()) {
                bestIndex = i;
            }
        }
        if (bestIndex < 0) {
            for (int i = 0; i < wordSpeakers.size(); ++i) {
                if (!wordSpeakers.at(i).isEmpty()) {
                    bestIndex = i;
                    break;
                }
            }
        }
        return bestIndex >= 0 && bestIndex < wordSpeakers.size()
            ? wordSpeakers.at(bestIndex).trimmed()
            : QString();
    }
    return QString();
}

ContinuityCoverageSummary continuityCoverageSummaryForClip(const QString& transcriptPath,
                                                          const QString& clipId)
{
    ContinuityCoverageSummary summary;
    if (transcriptPath.trimmed().isEmpty() || clipId.trimmed().isEmpty()) {
        return summary;
    }

    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (!engine.loadFacestreamArtifact(transcriptPath, &artifactRoot)) {
        return summary;
    }
    const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clipId);
    summary.importedArtifactDir =
        continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed();
    summary.runId = continuityRoot.value(QStringLiteral("run_id")).toString().trimmed();
    const QJsonArray rawTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    summary.rawTrackCount = rawTracks.size();
    qint64 minFrame = std::numeric_limits<qint64>::max();
    qint64 maxFrame = std::numeric_limits<qint64>::min();
    for (const QJsonValue& trackValue : rawTracks) {
        const QJsonArray detections =
            trackValue.toObject().value(QStringLiteral("detections")).toArray();
        for (const QJsonValue& detectionValue : detections) {
            const qint64 frame =
                detectionValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
            minFrame = qMin(minFrame, frame);
            maxFrame = qMax(maxFrame, frame);
        }
    }
    if (minFrame != std::numeric_limits<qint64>::max() &&
        maxFrame != std::numeric_limits<qint64>::min()) {
        summary.minRawTrackFrame = minFrame;
        summary.maxRawTrackFrame = maxFrame;
        summary.hasRawTrackCoverage = true;
    }
    return summary;
}

} // namespace

SpeakersTab::SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : TableTabBase(deps, parent)
    , m_widgets(widgets)
    , m_speakerDeps(deps)
{
    connect(&m_transcriptLoadWatcher, &QFutureWatcher<TranscriptDocumentLoadResult>::finished, this, [this]() {
        const TranscriptDocumentLoadResult result = m_transcriptLoadWatcher.result();
        if (!result.ok) {
            if (m_transcriptSession.matches(result.clipFilePath, result.transcriptPath) &&
                m_widgets.speakersInspectorDetailsLabel) {
                m_widgets.speakersInspectorDetailsLabel->setText(
                    result.error.isEmpty() ? QStringLiteral("Unable to load transcript JSON file.") : result.error);
            }
            m_updating = false;
            updateSelectedSpeakerPanel();
            updateSpeakerTrackingStatusLabel();
            return;
        }
        if (!m_transcriptSession.matches(result.clipFilePath, result.transcriptPath)) {
            return;
        }
        m_transcriptSession.assign(result.clipFilePath, result.transcriptPath, result.document);
        const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        if (!clip || clip->filePath != result.clipFilePath) {
            return;
        }
        applyLoadedTranscriptDocumentData(*clip, m_pendingPreferredSpeakerId);
    });
}

bool SpeakersTab::updateLoadedTranscriptDocument(const std::function<bool(QJsonObject&)>& mutator,
                                                 bool clearDerivedCaches)
{
    if (!m_transcriptSession.mutateRoot(mutator)) {
        return false;
    }
    if (clearDerivedCaches) {
        clearFaceDetectionsDerivedCaches();
    }
    return true;
}

bool SpeakersTab::saveLoadedTranscriptDocument()
{
    if (m_transcriptSession.transcriptPath().trimmed().isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    queueLoadedTranscriptDocumentSave();
    return true;
}

void SpeakersTab::queueLoadedTranscriptDocumentSave()
{
    if (m_transcriptSession.transcriptPath().trimmed().isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    m_transcriptSession.queueSave(
        shouldUseSynchronousTranscriptIo(),
        [](const QString& path, const QJsonDocument& doc) {
            editor::TranscriptEngine engine;
            engine.saveTranscriptJson(path, doc);
        });
}

void SpeakersTab::startTranscriptLoadRequest(const QString& clipFilePath,
                                             const QString& transcriptPath,
                                             const QString& preferredSpeakerId)
{
    if (clipFilePath.trimmed().isEmpty() || transcriptPath.trimmed().isEmpty()) {
        return;
    }
    m_pendingPreferredSpeakerId = preferredSpeakerId;
    if (shouldUseSynchronousTranscriptIo()) {
        const TranscriptDocumentLoadResult result = loadTranscriptDocumentResultCached(
            clipFilePath,
            transcriptPath,
            QStringLiteral("Unable to load transcript JSON file."));
        if (result.ok) {
            m_transcriptSession.assign(clipFilePath, transcriptPath, result.document);
            const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (clip && clip->filePath == clipFilePath) {
                applyLoadedTranscriptDocumentData(*clip, preferredSpeakerId);
            }
        } else {
            if (m_widgets.speakersInspectorClipLabel) {
                m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1")
                                                                  .arg(m_deps.getSelectedClip ? m_deps.getSelectedClip()->label : QString()));
            }
            if (m_widgets.speakersInspectorDetailsLabel) {
                m_widgets.speakersInspectorDetailsLabel->setText(result.error);
            }
            m_updating = false;
            updateSelectedSpeakerPanel();
            updateSpeakerTrackingStatusLabel();
        }
        return;
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        m_widgets.speakersInspectorDetailsLabel->setText(QStringLiteral("Loading transcript..."));
    }
    m_transcriptLoadWatcher.setFuture(QtConcurrent::run([clipFilePath, transcriptPath]() {
        return loadTranscriptDocumentResultCached(
            clipFilePath,
            transcriptPath,
            QStringLiteral("Unable to load transcript JSON file."));
    }));
}

void SpeakersTab::applyLoadedTranscriptDocumentData(const TimelineClip& clip, const QString& preferredSpeakerId)
{
    if (m_widgets.speakersInspectorClipLabel) {
        m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip.label));
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        const bool sidecarPresent = facedetectionsSidecarExistsForClipFile(clip.filePath);
        const bool mutableCut = activeCutMutable();
        m_widgets.speakersInspectorDetailsLabel->setText(
            QStringLiteral("%1 | Transcript: %2 | Edit mode: %3")
                .arg(QStringLiteral("Speaker workflow ready"))
                .arg(sidecarPresent ? QStringLiteral("Present") : QStringLiteral("Missing"))
                .arg(mutableCut ? QStringLiteral("Derived cut") : QStringLiteral("Original cut (read-only)")));
    }
    refreshTranscriptSpeakerViews(preferredSpeakerId, true);
    m_updating = false;
}

void SpeakersTab::refreshTranscriptSpeakerViews(const QString& preferredSpeakerId,
                                                bool refreshTrackPanels)
{
    refreshSpeakersTable(m_transcriptSession.rootObject(), preferredSpeakerId);
    refreshSpeakerSectionsTable(m_transcriptSession.rootObject());
    if (refreshTrackPanels) {
        requestRefreshFaceDetectionsPathsPanel();
    }
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }
    updateSelectedSpeakerPanel();
    updateSpeakerTrackingStatusLabel();
}

bool SpeakersTab::clipSupportsTranscript(const TimelineClip& clip) const
{
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}

bool SpeakersTab::rebuildProcessedFaceDetectionsForSelectedClip(bool interactive)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        if (interactive) {
            QMessageBox::information(nullptr,
                                     QStringLiteral("Rebuild Processed FaceDetections"),
                                     QStringLiteral("Select a clip first."));
        }
        return false;
    }
    if (m_transcriptSession.transcriptPath().trimmed().isEmpty()) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceDetections"),
                                 QStringLiteral("No active transcript is loaded for this clip."));
        }
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject rawArtifactRoot;
    if (!engine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &rawArtifactRoot)) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceDetections"),
                                 QStringLiteral("No raw FaceDetections artifact was found for this transcript."));
        }
        return false;
    }

    const QString clipId = clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id.trimmed();
    QJsonObject byClip = continuityFacestreamsByClipObject(rawArtifactRoot);
    QJsonObject continuityRoot = byClip.value(clipId).toObject();
    if (continuityRoot.isEmpty()) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceDetections"),
                                 QStringLiteral("No raw FaceDetections payload was found for the selected clip."));
        }
        return false;
    }

    continuityRoot[QStringLiteral("clip_id")] = clipId;
    continuityRoot[QStringLiteral("processed_artifact_path")] =
        engine.facedetectionsProcessedArtifactPath(m_transcriptSession.transcriptPath());
    byClip[clipId] = continuityRoot;
    setContinuityFacestreamsByClipObject(&rawArtifactRoot, byClip);
    if (!engine.saveFacestreamArtifact(m_transcriptSession.transcriptPath(), rawArtifactRoot)) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceDetections"),
                                 QStringLiteral("Failed to update raw FaceDetections artifact metadata."));
        }
        return false;
    }

    if (!jcut::facedetections::saveProcessedContinuityArtifact(
            m_transcriptSession.transcriptPath(),
            clipId,
            continuityRoot,
            m_transcriptSession.rootObject(),
            nullptr)) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceDetections"),
                                 QStringLiteral("Failed to rebuild processed FaceDetections sidecar."));
        }
        return false;
    }

    requestRefreshFaceDetectionsPathsPanel();
    if (m_speakerDeps.refreshPreview) {
        m_speakerDeps.refreshPreview();
    }
    return true;
}


QString SpeakersTab::originalTranscriptPathForClip(const QString& clipFilePath) const
{
    return transcriptPathForClipFile(clipFilePath);
}

bool SpeakersTab::activeCutMutable() const
{
    if (m_transcriptSession.transcriptPath().isEmpty() || m_transcriptSession.clipFilePath().isEmpty()) {
        return false;
    }
    return m_transcriptSession.transcriptPath() !=
           originalTranscriptPathForClip(m_transcriptSession.clipFilePath());
}

void SpeakersTab::refresh()
{
    m_updating = true;
    hideSpeakerAvatarHoverPreview();
    const QString selectedSpeakerBeforeClear = selectedSpeakerId();
    const QString preferredSpeakerId =
        selectedSpeakerBeforeClear.isEmpty() ? m_lastSelectedSpeakerIdHint : selectedSpeakerBeforeClear;
    const QString previousTranscriptPath = m_transcriptSession.transcriptPath();
    const QString previousClipFilePath = m_transcriptSession.clipFilePath();
    m_lastSelectionSeekSpeakerId.clear();
    m_lastSelectionSeekClipId.clear();
    m_lastPlayheadSyncedSpeakerId.clear();
    m_lastPlayheadSyncedSourceFrame = -1;

    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->clearContents();
        m_widgets.speakersTable->setRowCount(0);
        m_widgets.speakersTable->setEnabled(false);
        m_widgets.speakersTable->setIconSize(QSize(28, 28));
    }
    requestRefreshFaceDetectionsPathsPanel();

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip)) {
        m_transcriptSession.clear();
        m_speakersTableRefreshSignature.clear();
        if (m_widgets.speakersInspectorClipLabel) {
            m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("No clip selected"));
        }
        if (m_widgets.speakersInspectorDetailsLabel) {
            m_widgets.speakersInspectorDetailsLabel->setText(
                QStringLiteral("Select a clip with a transcript to review speakers, generate continuity tracks, and bind tracks to identities."));
        }
        m_updating = false;
        updateSelectedSpeakerPanel();
        updateSpeakerTrackingStatusLabel();
        return;
    }

    const QString transcriptPath =
        transcriptPathForRuntimeSidecarForClipFile(clip->filePath, activeTranscriptPathForClipFile(clip->filePath));
    const bool transcriptChanged =
        previousTranscriptPath != transcriptPath || previousClipFilePath != clip->filePath;
    if (transcriptChanged) {
        clearFaceDetectionsDerivedCaches();
        m_faceStreamPanelRefreshSignature.clear();
    }

    const bool canReuseLoadedDoc =
        m_transcriptSession.hasObjectDocument() &&
        m_transcriptSession.matches(clip->filePath, transcriptPath);
    if (!canReuseLoadedDoc) {
        m_transcriptSession.assign(clip->filePath, transcriptPath, QJsonDocument());
        startTranscriptLoadRequest(clip->filePath, transcriptPath, preferredSpeakerId);
        return;
    }
    applyLoadedTranscriptDocumentData(*clip, preferredSpeakerId);
}

void SpeakersTab::refreshForSubtab(const QString& subtabName)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const QString transcriptPath = clip
        ? transcriptPathForRuntimeSidecarForClipFile(
              clip->filePath,
              activeTranscriptPathForClipFile(clip->filePath))
        : QString();
    if (!clip || !clipSupportsTranscript(*clip) || !m_transcriptSession.hasObjectDocument() ||
        !m_transcriptSession.matches(clip->filePath, transcriptPath)) {
        refresh();
        return;
    }

    const QString normalized = subtabName.trimmed();
    if (normalized.compare(QStringLiteral("Continuity Tracks"), Qt::CaseInsensitive) == 0) {
        requestRefreshFaceDetectionsPathsPanel();
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (normalized.compare(QStringLiteral("Debug"), Qt::CaseInsensitive) == 0) {
        updateSpeakerTrackingStatusLabel();
        return;
    }

    if (m_widgets.speakersTable && m_widgets.speakersTable->rowCount() == 0) {
        refreshSpeakersTable(m_transcriptSession.rootObject());
    }
    if (m_widgets.speakerShowContiguousSectionsCheckBox &&
        m_widgets.speakerShowContiguousSectionsCheckBox->isChecked() &&
        m_widgets.speakerSectionsTable &&
        m_widgets.speakerSectionsTable->rowCount() == 0) {
        refreshSpeakerSectionsTable(m_transcriptSession.rootObject());
    }
    updateSelectedSpeakerPanel();
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::syncSpeakerListMode()
{
    const bool showSections =
        m_widgets.speakerShowContiguousSectionsCheckBox &&
        m_widgets.speakerShowContiguousSectionsCheckBox->isChecked();
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setVisible(!showSections);
    }
    if (m_widgets.speakerSectionsTable) {
        m_widgets.speakerSectionsTable->setVisible(showSections);
    }
}

void SpeakersTab::syncIdentityToPlayhead(int64_t absolutePlaybackSample,
                                         double sourceSeconds,
                                         int64_t sourceFrame)
{
    Q_UNUSED(absolutePlaybackSample);
    Q_UNUSED(sourceSeconds);
    if (m_updating || !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip)) {
        return;
    }
    if (sourceFrame < 0) {
        sourceFrame = currentSourceFrameForClip(*clip);
    }
    if (sourceFrame < 0 || sourceFrame == m_lastPlayheadSyncedSourceFrame) {
        return;
    }

    const QString activeSpeakerId =
        activeSpeakerIdInTranscriptRootAtSourceFrame(m_transcriptSession.rootObject(), sourceFrame);
    if (activeSpeakerId.isEmpty()) {
        m_lastPlayheadSyncedSourceFrame = sourceFrame;
        return;
    }

    const bool sameSpeaker = activeSpeakerId == m_lastPlayheadSyncedSpeakerId;
    const bool sectionSelected = selectSpeakerSectionRowAtFrame(activeSpeakerId, sourceFrame);
    const bool speakerSelected = selectSpeakerRowById(activeSpeakerId);
    if (!speakerSelected && !sectionSelected) {
        m_lastPlayheadSyncedSourceFrame = sourceFrame;
        return;
    }

    m_lastPlayheadSyncedSpeakerId = activeSpeakerId;
    m_lastPlayheadSyncedSourceFrame = sourceFrame;
    m_lastSelectedSpeakerIdHint = activeSpeakerId;
    if (!sameSpeaker) {
        updateSpeakerTrackingStatusLabelFast();
        updateSelectedSpeakerPanelFast();
    }
    syncCurrentSpeakerSentenceToPlayhead(true);
}

bool SpeakersTab::selectSpeakerRowById(const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || !m_widgets.speakersTable) {
        return false;
    }
    QSignalBlocker blocker(m_widgets.speakersTable);
    for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
        QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(row, 1);
        if (!speakerItem) {
            continue;
        }
        if (speakerItem->data(Qt::UserRole).toString().trimmed() != trimmedSpeakerId) {
            continue;
        }
        m_widgets.speakersTable->setCurrentCell(row, 1);
        m_widgets.speakersTable->selectRow(row);
        return true;
    }
    return false;
}

bool SpeakersTab::selectSpeakerSectionRowAtFrame(const QString& speakerId, int64_t sourceFrame)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || sourceFrame < 0 || !m_widgets.speakerSectionsTable) {
        return false;
    }
    QSignalBlocker blocker(m_widgets.speakerSectionsTable);
    for (int row = 0; row < m_widgets.speakerSectionsTable->rowCount(); ++row) {
        QTableWidgetItem* speakerItem = m_widgets.speakerSectionsTable->item(row, 1);
        if (!speakerItem) {
            continue;
        }
        const QString rowSpeakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
        const int64_t startFrame = speakerItem->data(Qt::UserRole + 1).toLongLong();
        const int64_t endFrame = speakerItem->data(Qt::UserRole + 2).toLongLong();
        if (rowSpeakerId != trimmedSpeakerId || startFrame < 0 || endFrame < startFrame ||
            sourceFrame < startFrame || sourceFrame > endFrame) {
            continue;
        }
        if (m_widgets.speakerSectionsTable->currentRow() != row) {
            m_widgets.speakerSectionsTable->setCurrentCell(row, 1);
            m_widgets.speakerSectionsTable->selectRow(row);
            m_widgets.speakerSectionsTable->scrollToItem(
                speakerItem, QAbstractItemView::PositionAtCenter);
        }
        return true;
    }
    return false;
}

void SpeakersTab::refreshSpeakerSectionsTable(const QJsonObject& transcriptRoot)
{
    if (!m_widgets.speakerSectionsTable) {
        return;
    }

    QString selectedSectionSpeakerId;
    int64_t selectedSectionStartTimelineFrame = -1;
    const int selectedTableRow = m_widgets.speakerSectionsTable->currentRow();
    if (selectedTableRow >= 0) {
        if (QTableWidgetItem* currentSpeakerItem = m_widgets.speakerSectionsTable->item(selectedTableRow, 1)) {
            selectedSectionSpeakerId = currentSpeakerItem->data(Qt::UserRole).toString().trimmed();
            selectedSectionStartTimelineFrame =
                currentSpeakerItem->data(Qt::UserRole + 1).toLongLong();
        }
    }

    struct SpeakerSectionRow {
        QString speakerId;
        QString displayLabel;
        int64_t startTimelineFrame = -1;
        int64_t endTimelineFrame = -1;
        int wordCount = 0;
        QStringList snippetWords;
    };

    const QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    QVector<SpeakerSectionRow> rows;
    SpeakerSectionRow currentRow;
    auto flushCurrentRow = [&rows, &currentRow]() {
        if (currentRow.speakerId.isEmpty()) {
            return;
        }
        rows.push_back(currentRow);
        currentRow = SpeakerSectionRow{};
    };

    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        if (!words.isEmpty()) {
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    continue;
                }
                QString speakerId =
                    wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (speakerId.isEmpty()) {
                    speakerId = segmentSpeaker;
                }
                if (speakerId.isEmpty()) {
                    flushCurrentRow();
                    continue;
                }
                const QString text = wordObj.value(QStringLiteral("text")).toString().trimmed();
                const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double endSeconds =
                    wordObj.value(QStringLiteral("end")).toDouble(startSeconds);
                const int64_t startFrame =
                    startSeconds >= 0.0
                        ? qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)))
                        : -1;
                const int64_t endFrame =
                    endSeconds >= 0.0
                        ? qMax<int64_t>(0, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)))
                        : startFrame;
                if (currentRow.speakerId != speakerId) {
                    flushCurrentRow();
                    currentRow.speakerId = speakerId;
                    const QString displayName =
                        profiles.value(speakerId).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
                    currentRow.displayLabel = displayName.isEmpty() ? speakerId : displayName;
                    currentRow.startTimelineFrame = startFrame;
                    currentRow.endTimelineFrame = endFrame;
                } else if (endFrame >= 0) {
                    currentRow.endTimelineFrame = qMax(currentRow.endTimelineFrame, endFrame);
                }
                if (currentRow.startTimelineFrame < 0 && startFrame >= 0) {
                    currentRow.startTimelineFrame = startFrame;
                }
                ++currentRow.wordCount;
                if (!text.isEmpty() && currentRow.snippetWords.size() < 14) {
                    currentRow.snippetWords.push_back(text);
                }
            }
            continue;
        }

        if (segmentSpeaker.isEmpty()) {
            flushCurrentRow();
            continue;
        }
        const QString text = segmentObj.value(QStringLiteral("text")).toString().simplified();
        const double startSeconds = segmentObj.value(QStringLiteral("start")).toDouble(-1.0);
        const double endSeconds = segmentObj.value(QStringLiteral("end")).toDouble(startSeconds);
        const int64_t startFrame =
            startSeconds >= 0.0
                ? qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)))
                : -1;
        const int64_t endFrame =
            endSeconds >= 0.0
                ? qMax<int64_t>(0, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)))
                : startFrame;
        if (currentRow.speakerId != segmentSpeaker) {
            flushCurrentRow();
            currentRow.speakerId = segmentSpeaker;
            const QString displayName =
                profiles.value(segmentSpeaker).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
            currentRow.displayLabel = displayName.isEmpty() ? segmentSpeaker : displayName;
            currentRow.startTimelineFrame = startFrame;
            currentRow.endTimelineFrame = endFrame;
        } else if (endFrame >= 0) {
            currentRow.endTimelineFrame = qMax(currentRow.endTimelineFrame, endFrame);
        }
        if (currentRow.startTimelineFrame < 0 && startFrame >= 0) {
            currentRow.startTimelineFrame = startFrame;
        }
        ++currentRow.wordCount;
        if (!text.isEmpty()) {
            const QStringList wordsForSnippet = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString& word : wordsForSnippet) {
                if (currentRow.snippetWords.size() >= 14) {
                    break;
                }
                currentRow.snippetWords.push_back(word);
            }
        }
    }
    flushCurrentRow();

    QSignalBlocker blocker(m_widgets.speakerSectionsTable);
    m_widgets.speakerSectionsTable->clearContents();
    m_widgets.speakerSectionsTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const SpeakerSectionRow& section = rows.at(row);
        const QString rangeText =
            section.startTimelineFrame >= 0 && section.endTimelineFrame >= 0
                ? QStringLiteral("%1-%2").arg(section.startTimelineFrame).arg(section.endTimelineFrame)
                : QStringLiteral("-");
        QString snippet = section.snippetWords.join(QLatin1Char(' ')).simplified();
        if (section.wordCount > section.snippetWords.size()) {
            snippet += QStringLiteral(" ...");
        }
        auto* indexItem = new QTableWidgetItem(QString::number(row + 1));
        auto* speakerItem = new QTableWidgetItem(section.displayLabel);
        auto* rangeItem = new QTableWidgetItem(rangeText);
        auto* wordsItem = new QTableWidgetItem(QString::number(section.wordCount));
        auto* snippetItem = new QTableWidgetItem(snippet);
        speakerItem->setData(Qt::UserRole, section.speakerId);
        speakerItem->setData(Qt::UserRole + 1, QVariant::fromValue<qlonglong>(section.startTimelineFrame));
        speakerItem->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(section.endTimelineFrame));
        speakerItem->setToolTip(QStringLiteral("Speaker ID: %1").arg(section.speakerId));
        indexItem->setFlags(indexItem->flags() & ~Qt::ItemIsEditable);
        speakerItem->setFlags(speakerItem->flags() & ~Qt::ItemIsEditable);
        rangeItem->setFlags(rangeItem->flags() & ~Qt::ItemIsEditable);
        wordsItem->setFlags(wordsItem->flags() & ~Qt::ItemIsEditable);
        snippetItem->setFlags(snippetItem->flags() & ~Qt::ItemIsEditable);
        m_widgets.speakerSectionsTable->setItem(row, 0, indexItem);
        m_widgets.speakerSectionsTable->setItem(row, 1, speakerItem);
        m_widgets.speakerSectionsTable->setItem(row, 2, rangeItem);
        m_widgets.speakerSectionsTable->setItem(row, 3, wordsItem);
        m_widgets.speakerSectionsTable->setItem(row, 4, snippetItem);
    }
    if (!rows.isEmpty()) {
        int rowToSelect = 0;
        if (!selectedSectionSpeakerId.isEmpty()) {
            for (int row = 0; row < rows.size(); ++row) {
                const SpeakerSectionRow& section = rows.at(row);
                if (section.speakerId == selectedSectionSpeakerId &&
                    section.startTimelineFrame == selectedSectionStartTimelineFrame) {
                    rowToSelect = row;
                    break;
                }
            }
        }
        m_widgets.speakerSectionsTable->setCurrentCell(rowToSelect, 0);
    }
}

bool SpeakersTab::generateFaceDetectionsForSelectedClip()
{
    if (!activeCutMutable()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("JCut DNN FaceDetections Generator"),
            QStringLiteral("FaceDetections actions are editable only on derived cuts (not Original)."));
        return false;
    }
    onSpeakerRunAutoTrackClicked();
    return true;
}

editor::ActionResult SpeakersTab::deleteFaceDetectionsForSelectedClipResult(bool confirmDialog,
                                                                        bool interactive)
{
    auto maybeShow = [interactive](auto dialogFn, const QString& message) {
        if (interactive) {
            dialogFn(message);
        }
    };
    auto fail = [&maybeShow](const QString& code,
                             const QString& message,
                             auto dialogFn,
                             const QJsonObject& details = QJsonObject{}) -> editor::ActionResult {
        maybeShow(dialogFn, message);
        return editor::ActionResult::failure(code, message, details);
    };

    if (!activeCutMutable()) {
        return fail(
            QStringLiteral("immutable_cut"),
            QStringLiteral("FaceDetections actions are editable only on derived cuts (not Original)."),
            [](const QString& message) {
                QMessageBox::information(
                    nullptr,
                    QStringLiteral("Delete FaceDetections"),
                    message);
            });
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return editor::ActionResult::failure(
            QStringLiteral("no_selected_clip"),
            QStringLiteral("No clip is selected."));
    }

    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    engine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot);
    QJsonObject continuityByClip = continuityFacestreamsByClipObject(artifactRoot);
    const QString clipId = clip->id.trimmed();
    const QJsonObject continuityRoot = continuityByClip.value(clipId).toObject();
    const QJsonArray streams = jcut::facedetections::continuityStreamsForRoot(
        continuityRoot,
        m_transcriptSession.rootObject());
    const bool hasStoredPayload =
        jcut::facedetections::continuityRootHasStoredPayload(continuityRoot);
    QString facedetectionsPartPath = continuityRoot.value(QStringLiteral("facedetections_part")).toString().trimmed();
    if (facedetectionsPartPath.isEmpty()) {
        const QString importedArtifactDir =
            continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed();
        if (!importedArtifactDir.isEmpty()) {
            facedetectionsPartPath =
                QDir(importedArtifactDir).absoluteFilePath(QStringLiteral("facedetections.part"));
        }
    }

    if (streams.isEmpty() && !hasStoredPayload) {
        return fail(
            QStringLiteral("no_facedetections_paths"),
            QStringLiteral("No FaceDetections paths were found for this clip."),
            [](const QString& message) {
                QMessageBox::information(
                    nullptr,
                    QStringLiteral("Delete FaceDetections"),
                    message);
            });
    }

    bool deleteFacestreamPart = false;
    if (confirmDialog) {
        QStringList affectedPaths;
        const QString facedetectionsArtifactPath = engine.facedetectionsArtifactPath(m_transcriptSession.transcriptPath());
        if (!facedetectionsArtifactPath.trimmed().isEmpty()) {
            affectedPaths.push_back(facedetectionsArtifactPath);
        }
        const QString processedArtifactPath = engine.facedetectionsProcessedArtifactPath(m_transcriptSession.transcriptPath());
        QJsonObject processedArtifactRoot;
        const bool hasProcessedClipPayload =
            engine.loadFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), &processedArtifactRoot) &&
            jcut::facedetections::continuityRootHasStoredPayload(
                continuityRootForClip(processedArtifactRoot, clipId));
        if (hasProcessedClipPayload && !processedArtifactPath.trimmed().isEmpty()) {
            affectedPaths.push_back(processedArtifactPath);
        }
        affectedPaths.removeDuplicates();

        QDialog confirmationDialog;
        confirmationDialog.setWindowTitle(QStringLiteral("Delete FaceDetections"));
        confirmationDialog.setWindowFlag(Qt::Window, true);
        confirmationDialog.resize(620, 280);

        auto* layout = new QVBoxLayout(&confirmationDialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        QString confirmationMessage =
            QStringLiteral("Delete FaceDetections data for this clip?\n\n");
        if (!affectedPaths.isEmpty()) {
            confirmationMessage += QStringLiteral("This will remove this clip's FaceDetections entries from:\n");
            for (const QString& path : affectedPaths) {
                confirmationMessage += QStringLiteral("- %1\n").arg(path);
            }
            confirmationMessage += QLatin1Char('\n');
        }
        confirmationMessage += QStringLiteral(
            "By default, debug-run artifacts such as facedetections.part, tracks.bin, continuity_facedetections.bin, and summary.json are not deleted by this action.\n\nThis cannot be undone.");

        auto* label = new QLabel(confirmationMessage, &confirmationDialog);
        label->setWordWrap(true);
        layout->addWidget(label);

        QCheckBox* deletePartCheckbox = nullptr;
        if (!facedetectionsPartPath.isEmpty()) {
            deletePartCheckbox = new QCheckBox(
                QStringLiteral("Also delete facedetections.part checkpoint"), &confirmationDialog);
            deletePartCheckbox->setToolTip(facedetectionsPartPath);
            layout->addWidget(deletePartCheckbox);

            auto* partPathLabel = new QLabel(
                QStringLiteral("Checkpoint: %1").arg(facedetectionsPartPath), &confirmationDialog);
            partPathLabel->setWordWrap(true);
            partPathLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
            layout->addWidget(partPathLabel);
        }

        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &confirmationDialog);
        auto* deleteButton = new QPushButton(QStringLiteral("Delete"), &confirmationDialog);
        buttons->addWidget(cancelButton);
        buttons->addWidget(deleteButton);
        layout->addLayout(buttons);

        QObject::connect(cancelButton, &QPushButton::clicked, &confirmationDialog, &QDialog::reject);
        QObject::connect(deleteButton, &QPushButton::clicked, &confirmationDialog, &QDialog::accept);

        if (confirmationDialog.exec() != QDialog::Accepted) {
            return editor::ActionResult::failure(
                QStringLiteral("canceled"),
                QStringLiteral("Delete FaceDetections was canceled."));
        }
        deleteFacestreamPart = deletePartCheckbox && deletePartCheckbox->isChecked();
    }

    continuityByClip.remove(clipId);
    setContinuityFacestreamsByClipObject(&artifactRoot, continuityByClip);
    const bool savedArtifact = engine.saveFacestreamArtifact(m_transcriptSession.transcriptPath(), artifactRoot);
    if (!savedArtifact) {
        return fail(
            QStringLiteral("save_failed"),
            QStringLiteral("Failed to save FaceDetections artifact after deletion."),
            [](const QString& message) {
                QMessageBox::warning(
                    nullptr,
                    QStringLiteral("Delete FaceDetections"),
                    message);
            });
    }

    QJsonObject processedArtifactRoot;
    if (engine.loadFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), &processedArtifactRoot)) {
        QJsonObject processedByClip = continuityFacestreamsByClipObject(processedArtifactRoot);
        if (processedByClip.contains(clipId)) {
            processedByClip.remove(clipId);
            setContinuityFacestreamsByClipObject(&processedArtifactRoot, processedByClip);
            engine.saveFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), processedArtifactRoot);
        }
    }

    if (deleteFacestreamPart && !facedetectionsPartPath.isEmpty() && QFileInfo::exists(facedetectionsPartPath)) {
        if (!QFile::remove(facedetectionsPartPath)) {
            return fail(
                QStringLiteral("delete_checkpoint_failed"),
                QStringLiteral("FaceDetections entries were removed, but deleting facedetections.part failed:\n%1")
                    .arg(facedetectionsPartPath),
                [](const QString& message) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("Delete FaceDetections"),
                        message);
                });
        }
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    return editor::ActionResult::success();
}

bool SpeakersTab::deleteFaceDetectionsForSelectedClip(bool confirmDialog, QString* errorOut)
{
    const editor::ActionResult result =
        deleteFaceDetectionsForSelectedClipResult(confirmDialog, true);
    if (!result.ok && errorOut) {
        *errorOut = result.message;
    }
    return result.ok;
}

bool SpeakersTab::selectedClipHasFaceDetectionsSidecars() const
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || m_transcriptSession.transcriptPath().trimmed().isEmpty()) {
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip->id);
        if (jcut::facedetections::continuityRootHasStoredPayload(continuityRoot)) {
            return true;
        }
    }

    QJsonObject processedArtifactRoot;
    if (engine.loadFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), &processedArtifactRoot)) {
        if (jcut::facedetections::continuityRootHasStoredPayload(
                continuityRootForClip(processedArtifactRoot, clip->id))) {
            return true;
        }
    }

    return false;
}

void SpeakersTab::onSpeakerViewFaceDetectionsClicked()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        QMessageBox::information(nullptr, QStringLiteral("View FaceDetections"), QStringLiteral("Select a clip first."));
        return;
    }

    QString text;
    text += QStringLiteral("Selected clip: %1\n").arg(clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id);
    text += QStringLiteral("Transcript artifact: %1\n\n").arg(m_transcriptSession.transcriptPath());

    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    const bool loadedArtifact = transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot);
    QJsonObject continuityRoot;
    if (loadedArtifact) {
        continuityRoot = continuityRootForClip(artifactRoot, clip->id);
    }
    const QJsonArray streams = jcut::facedetections::continuityStreamsForRoot(
        continuityRoot,
        m_transcriptSession.rootObject());
    text += QStringLiteral("Imported streams: %1\n").arg(streams.size());
    text += QStringLiteral("Raw tracks: %1\n")
                .arg(continuityRoot.value(QStringLiteral("raw_tracks")).toArray().size());
    const QString importedArtifactDir = continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString();
    if (!importedArtifactDir.isEmpty()) {
        text += QStringLiteral("Imported artifact dir: %1\n").arg(importedArtifactDir);
    }
    QString facedetectionsPath = continuityRoot.value(QStringLiteral("facedetections_part")).toString();
    if (facedetectionsPath.isEmpty()) {
        facedetectionsPath = continuityRoot.value(QStringLiteral("facedetections_bin")).toString();
    }
    if (facedetectionsPath.isEmpty()) {
        facedetectionsPath = continuityRoot.value(QStringLiteral("facedetections_ndjson")).toString();
    }
    if (!facedetectionsPath.isEmpty()) {
        const QFileInfo streamInfo(facedetectionsPath);
        text += QStringLiteral("facedetections checkpoint: %1 (%2 bytes)\n")
                    .arg(facedetectionsPath)
                    .arg(streamInfo.exists() ? streamInfo.size() : -1);
    }
    const QString processedPath = transcriptEngine.facedetectionsProcessedArtifactPath(m_transcriptSession.transcriptPath());
    if (!processedPath.isEmpty()) {
        const QFileInfo processedInfo(processedPath);
        text += QStringLiteral("processed sidecar: %1 (%2 bytes)\n")
                    .arg(processedPath)
                    .arg(processedInfo.exists() ? processedInfo.size() : -1);
    }
    const QString summaryPath = continuityRoot.value(QStringLiteral("summary_json")).toString();
    if (!summaryPath.isEmpty()) {
        text += QStringLiteral("summary.json: %1\n").arg(summaryPath);
    }
    const QString identitySidecarPath = transcriptEngine.identityArtifactPath(m_transcriptSession.transcriptPath());
    if (!identitySidecarPath.isEmpty()) {
        const QFileInfo identityInfo(identitySidecarPath);
        text += QStringLiteral("identity sidecar: %1")
                    .arg(identityInfo.exists() ? identitySidecarPath : QStringLiteral("missing"));
        if (identityInfo.exists()) {
            text += QStringLiteral(" (%1 bytes)").arg(identityInfo.size());
        }
        text += QLatin1Char('\n');
    }

    const QString clipToken = speaker_flow_debug::sanitizeToken(
        clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id);
    const QString projectRoot = speaker_flow_debug::deriveProjectRootFromTranscriptPath(m_transcriptSession.transcriptPath());
    const QString debugRoot = QDir(projectRoot).absoluteFilePath(QStringLiteral("debug/speaker_flow/%1").arg(clipToken));
    const QString latestRun = speaker_flow_debug::latestRunIdWithArtifact(debugRoot);
    if (!latestRun.isEmpty()) {
        const QString runDir = QDir(debugRoot).absoluteFilePath(latestRun);
        const QString artifactDir = QDir(runDir).absoluteFilePath(QStringLiteral("facedetections_artifact"));
        text += QStringLiteral("\nLatest debug run: %1\n").arg(runDir);
        text += QStringLiteral("Latest artifact dir: %1\n").arg(artifactDir);
        const QStringList artifactFiles{
            QStringLiteral("facedetections.part"),
            QStringLiteral("tracks.bin"),
            QStringLiteral("continuity_facedetections.bin"),
            QStringLiteral("summary.json")
        };
        for (const QString& fileName : artifactFiles) {
            const QString path = QDir(artifactDir).absoluteFilePath(fileName);
            const QFileInfo info(path);
            text += QStringLiteral("- %1: %2")
                        .arg(fileName, info.exists() ? path : QStringLiteral("missing"));
            if (info.exists()) {
                text += QStringLiteral(" (%1 bytes)").arg(info.size());
            }
            text += QLatin1Char('\n');
        }
    } else {
        text += QStringLiteral("\nLatest generated artifact: none found for this clip.\n");
        if (QFileInfo::exists(debugRoot)) {
            text += QStringLiteral("Debug root exists but contains no facedetections_artifact files: %1\n").arg(debugRoot);
        }
    }

    text += QStringLiteral("\n");
    const int row = m_widgets.speakerFaceDetectionsTable ? m_widgets.speakerFaceDetectionsTable->currentRow() : -1;
    if (row >= 0 && row < m_faceStreamPanelRows.size()) {
        text += QStringLiteral("Selected stream:\n");
        text += QString::fromUtf8(QJsonDocument(m_faceStreamPanelRows.at(row).toObject()).toJson(QJsonDocument::Indented));
    } else if (!streams.isEmpty()) {
        text += QStringLiteral("All imported streams:\n");
        text += QString::fromUtf8(QJsonDocument(streams).toJson(QJsonDocument::Indented));
    } else {
        text += QStringLiteral("No imported FaceDetections paths found for this clip.");
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("View FaceDetections"));
    dialog.setWindowFlag(Qt::Window, true);
    dialog.resize(900, 650);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(QStringLiteral("Generated artifact viewer. This shows imported continuity streams plus the latest resumable artifact file paths."), &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* edit = new QPlainTextEdit(&dialog);
    edit->setReadOnly(true);
    edit->setPlainText(text);
    layout->addWidget(edit, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), &dialog);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}


void SpeakersTab::refreshSpeakersTable(const QJsonObject& transcriptRoot,
                                       const QString& preferredSpeakerId)
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    auto finalizeRefreshTiming = [this, &refreshTimer]() {
        m_lastSpeakersTableRefreshDurationMs = refreshTimer.elapsed();
        m_maxSpeakersTableRefreshDurationMs =
            qMax(m_maxSpeakersTableRefreshDurationMs, m_lastSpeakersTableRefreshDurationMs);
    };
    if (!m_widgets.speakersTable) {
        finalizeRefreshTiming();
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        finalizeRefreshTiming();
        return;
    }
    const QFileInfo transcriptInfo(m_transcriptSession.transcriptPath());
    const qint64 artifactRevisionMs = facedetectionsArtifactRevisionMsForTranscript(m_transcriptSession.transcriptPath());
    const QString refreshSignature =
        clip->id + QLatin1Char('|') +
        m_transcriptSession.transcriptPath() + QLatin1Char('|') +
        QString::number(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0) +
        QLatin1Char('|') +
        QString::number(artifactRevisionMs) + QLatin1Char('|') +
        QString::number(m_widgets.speakerHideUnidentifiedCheckBox &&
                        m_widgets.speakerHideUnidentifiedCheckBox->isChecked());
    const QJsonArray streams = continuityStreamsForClip(*clip);
    const QHash<int, QString> identityByTrackId = resolvedIdentityByTrackId(*clip, streams);
    QHash<QString, QVector<int>> assignedTrackIdsBySpeaker;
    for (auto it = identityByTrackId.cbegin(); it != identityByTrackId.cend(); ++it) {
        assignedTrackIdsBySpeaker[it.value()].push_back(it.key());
    }

    QSet<QString> speakerIds;
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker = segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        if (!segmentSpeaker.isEmpty()) {
            speakerIds.insert(segmentSpeaker);
        }
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QString wordSpeaker =
                wordValue.toObject().value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (!wordSpeaker.isEmpty()) {
                speakerIds.insert(wordSpeaker);
            }
        }
    }

    const QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QStringList ids = speakerIds.values();
    if (m_widgets.speakerHideUnidentifiedCheckBox &&
        m_widgets.speakerHideUnidentifiedCheckBox->isChecked()) {
        ids.erase(std::remove_if(ids.begin(), ids.end(), [&profiles](const QString& id) {
                      return profiles.value(id)
                          .toObject()
                          .value(QString(kTranscriptSpeakerNameKey))
                          .toString()
                          .trimmed()
                          .isEmpty();
                  }),
                  ids.end());
    }
    std::sort(ids.begin(), ids.end(), [&profiles](const QString& a, const QString& b) {
        const QString aName =
            profiles.value(a).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
        const QString bName =
            profiles.value(b).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
        const QString aLabel = aName.isEmpty() ? a : aName;
        const QString bLabel = bName.isEmpty() ? b : bName;
        const int byLabel = aLabel.localeAwareCompare(bLabel);
        if (byLabel != 0) {
            return byLabel < 0;
        }
        return a.localeAwareCompare(b) < 0;
    });
    const QString selectionToRestore =
        preferredSpeakerId.isEmpty() ? selectedSpeakerId() : preferredSpeakerId;
    if (refreshSignature == m_speakersTableRefreshSignature &&
        m_widgets.speakersTable->rowCount() > 0) {
        if (!selectionToRestore.isEmpty()) {
            QSignalBlocker blocker(m_widgets.speakersTable);
            for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
                QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(row, 1);
                if (!speakerItem) {
                    continue;
                }
                const QString rowSpeakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
                if (rowSpeakerId != selectionToRestore) {
                    continue;
                }
                m_widgets.speakersTable->setCurrentCell(row, 1);
                m_widgets.speakersTable->selectRow(row);
                m_lastSelectedSpeakerIdHint = rowSpeakerId;
                break;
            }
        }
        finalizeRefreshTiming();
        return;
    }

    QSignalBlocker blocker(m_widgets.speakersTable);
    m_widgets.speakersTable->clearContents();
    m_widgets.speakersTable->setRowCount(ids.size());
    const bool playbackActive = m_speakerDeps.isPlaybackActive && m_speakerDeps.isPlaybackActive();
    const QString avatarMediaPath = interactivePreviewMediaPathForClip(*clip);
    std::unique_ptr<editor::DecoderContext> avatarDecoder;
    if (!playbackActive && !avatarMediaPath.isEmpty()) {
        avatarDecoder = std::make_unique<editor::DecoderContext>(avatarMediaPath);
        if (!avatarDecoder->initialize()) {
            avatarDecoder.reset();
        }
    }
    QHash<int64_t, QImage> avatarFrameImageCache;
    for (int row = 0; row < ids.size(); ++row) {
        const QString id = ids.at(row);
        const QJsonObject profileJson = profiles.value(id).toObject();
        const SpeakerProfile speakerProfile = speakerProfileFromJson(id, profileJson);
        const QString name = speakerProfile.name.trimmed();
        const QString displayLabel = name.isEmpty() ? id : name;
        const QString organization = speakerProfile.organization;
        const QString description = speakerProfile.description;
        const QJsonObject location = profileJson.value(QString(kTranscriptSpeakerLocationKey)).toObject();
        const double x = location.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5);
        const double y = location.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85);
        const QVector<int> assignedTrackIds = assignedTrackIdsBySpeaker.value(id);
        const int assignedTrackCount = assignedTrackIds.size();

        auto* avatarItem = new QTableWidgetItem();
        avatarItem->setFlags(avatarItem->flags() & ~Qt::ItemIsEditable);
        avatarItem->setData(Qt::UserRole, id);
        avatarItem->setIcon(QIcon(speakerAvatarForRow(
            *clip,
            transcriptRoot,
            profileJson,
            streams,
            id,
            assignedTrackIdsBySpeaker.value(id),
            avatarDecoder.get(),
            &avatarFrameImageCache)));
        const uint hueHash = qHash(id);
        const QColor speakerHueTint = QColor::fromHsv(static_cast<int>(hueHash % 360), 160, 92, 105);
        avatarItem->setBackground(QBrush(speakerHueTint));
        avatarItem->setToolTip(
            QStringLiteral("Primary avatar. Unset by default. Click avatar and square-select in Preview to set."));
        avatarItem->setTextAlignment(Qt::AlignCenter);
        avatarItem->setSizeHint(QSize(32, 32));

        auto* displayItem = new QTableWidgetItem(displayLabel);
        displayItem->setData(Qt::UserRole, id);
        displayItem->setData(Qt::UserRole + 10, assignedTrackCount > 0);
        displayItem->setToolTip(QStringLiteral("Speaker ID: %1\nOrganization: %2\nSummary: %3")
                                    .arg(id,
                                         organization.isEmpty() ? QStringLiteral("None") : organization,
                                         description.isEmpty() ? QStringLiteral("None") : description));
        auto* xItem = new QTableWidgetItem(QString::number(x, 'f', 3));
        xItem->setData(Qt::UserRole, id);
        auto* yItem = new QTableWidgetItem(QString::number(y, 'f', 3));
        yItem->setData(Qt::UserRole, id);
        auto* assignedTracksItem = new QTableWidgetItem(QString::number(assignedTrackCount));
        assignedTracksItem->setFlags(assignedTracksItem->flags() & ~Qt::ItemIsEditable);
        assignedTracksItem->setData(Qt::UserRole, id);
        assignedTracksItem->setTextAlignment(Qt::AlignCenter);
        assignedTracksItem->setToolTip(
            assignedTrackIds.isEmpty()
                ? QStringLiteral("No continuity tracks are assigned to this speaker yet.")
                : QStringLiteral("Resolved assigned continuity track IDs: %1")
                      .arg([&assignedTrackIds]() {
                          QStringList parts;
                          parts.reserve(assignedTrackIds.size());
                          for (int trackId : assignedTrackIds) {
                              parts.push_back(QString::number(trackId));
                          }
                          return parts.join(QStringLiteral(", "));
                      }()));
        auto* addTrackButton = new QToolButton(m_widgets.speakersTable);
        addTrackButton->setObjectName(QStringLiteral("speakers.roster.add_tracks.%1").arg(id));
        addTrackButton->setText(QStringLiteral("+"));
        addTrackButton->setToolTip(QStringLiteral("Assign continuity tracks to this speaker."));
        addTrackButton->setAutoRaise(false);
        addTrackButton->setMinimumSize(QSize(28, 28));
        connect(addTrackButton, &QToolButton::clicked, this, [this, id]() {
            selectSpeakerRowById(id);
            m_lastSelectedSpeakerIdHint = id;
            updateSpeakerTrackingStatusLabel();
            updateSelectedSpeakerPanel();
            if (!uiAutomationEnabled()) {
                if (!openPlayheadTrackPickerForSpeaker(id)) {
                    openTrackPickerForSpeaker(id);
                }
            }
        });
        m_widgets.speakersTable->setItem(row, 0, avatarItem);
        m_widgets.speakersTable->setItem(row, 1, displayItem);
        m_widgets.speakersTable->setItem(row, 2, xItem);
        m_widgets.speakersTable->setItem(row, 3, yItem);
        m_widgets.speakersTable->setItem(row, 4, assignedTracksItem);
        m_widgets.speakersTable->setCellWidget(row, 5, addTrackButton);
        m_widgets.speakersTable->setRowHeight(row, 34);
    }

    bool restoredSelection = false;
    if (!selectionToRestore.isEmpty()) {
        for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
            QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(row, 1);
            if (!speakerItem) {
                continue;
            }
            const QString rowSpeakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
            if (rowSpeakerId != selectionToRestore) {
                continue;
            }
            m_widgets.speakersTable->setCurrentCell(row, 1);
            m_widgets.speakersTable->selectRow(row);
            m_lastSelectedSpeakerIdHint = rowSpeakerId;
            restoredSelection = true;
            break;
        }
    }
    if (!restoredSelection && !ids.isEmpty()) {
        QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(0, 1);
        if (speakerItem) {
            m_widgets.speakersTable->setCurrentCell(0, 1);
            m_widgets.speakersTable->selectRow(0);
            m_lastSelectedSpeakerIdHint = speakerItem->data(Qt::UserRole).toString().trimmed();
        }
    }
    m_speakersTableRefreshSignature = refreshSignature;
    finalizeRefreshTiming();
}

QString SpeakersTab::selectedSpeakerId() const
{
    const auto speakerIdFromRow = [](QTableWidget* table, int row) {
        if (!table || row < 0) {
            return QString();
        }
        QTableWidgetItem* speakerItem = table->item(row, 1);
        return speakerItem ? speakerItem->data(Qt::UserRole).toString().trimmed() : QString();
    };

    if (m_widgets.speakersTable && m_widgets.speakersTable->selectionModel()) {
        const QModelIndexList selectedRows = m_widgets.speakersTable->selectionModel()->selectedRows();
        if (!selectedRows.isEmpty()) {
            const QString selectedId =
                speakerIdFromRow(m_widgets.speakersTable, selectedRows.constFirst().row());
            if (!selectedId.isEmpty()) {
                return selectedId;
            }
        }

        const QString currentId =
            speakerIdFromRow(m_widgets.speakersTable, m_widgets.speakersTable->currentRow());
        if (!currentId.isEmpty()) {
            return currentId;
        }
    }

    if (m_widgets.speakerSectionsTable && m_widgets.speakerSectionsTable->selectionModel()) {
        const QModelIndexList selectedRows = m_widgets.speakerSectionsTable->selectionModel()->selectedRows();
        if (!selectedRows.isEmpty()) {
            const QString selectedId =
                speakerIdFromRow(m_widgets.speakerSectionsTable, selectedRows.constFirst().row());
            if (!selectedId.isEmpty()) {
                return selectedId;
            }
        }

        const QString currentId =
            speakerIdFromRow(m_widgets.speakerSectionsTable, m_widgets.speakerSectionsTable->currentRow());
        if (!currentId.isEmpty()) {
            return currentId;
        }
    }

    return m_lastSelectedSpeakerIdHint.trimmed();
}

QString SpeakersTab::speakerDisplayName(const QString& speakerId) const
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return QString();
    }
    const QJsonObject profiles =
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    return profiles.value(trimmedSpeakerId).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
}

QString SpeakersTab::speakerDisplayLabel(const QString& speakerId) const
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty()) {
        return QString();
    }
    const QString name = speakerDisplayName(trimmedSpeakerId);
    return name.isEmpty() ? trimmedSpeakerId : name;
}

QString SpeakersTab::activeSpeakerIdAtSourceFrame(int64_t sourceFrame) const
{
    if (!m_transcriptSession.hasObjectDocument()) {
        return QString();
    }
    return activeSpeakerIdInTranscriptRootAtSourceFrame(
        m_transcriptSession.rootObject(), sourceFrame).trimmed();
}

QString SpeakersTab::activeSpeakerIdNearSourceFrame(int64_t sourceFrame,
                                                    int gapHoldFrames,
                                                    int64_t* resolvedSourceFrameOut) const
{
    if (resolvedSourceFrameOut) {
        *resolvedSourceFrameOut = sourceFrame;
    }
    if (sourceFrame < 0 || gapHoldFrames <= 0 || !m_transcriptSession.hasObjectDocument()) {
        return QString();
    }

    const int boundedGapHold = qBound(0, gapHoldFrames, 240);
    for (int offset = 1; offset <= boundedGapHold; ++offset) {
        const int64_t previousFrame = sourceFrame - offset;
        if (previousFrame >= 0) {
            const QString previousSpeaker = activeSpeakerIdAtSourceFrame(previousFrame);
            if (!previousSpeaker.isEmpty()) {
                if (resolvedSourceFrameOut) {
                    *resolvedSourceFrameOut = previousFrame;
                }
                return previousSpeaker;
            }
        }

        const QString nextSpeaker = activeSpeakerIdAtSourceFrame(sourceFrame + offset);
        if (!nextSpeaker.isEmpty()) {
            if (resolvedSourceFrameOut) {
                *resolvedSourceFrameOut = sourceFrame + offset;
            }
            return nextSpeaker;
        }
    }
    return QString();
}

int64_t SpeakersTab::firstSourceFrameForSpeaker(const QJsonObject& transcriptRoot,
                                                const QString& speakerId) const
{
    if (speakerId.trimmed().isEmpty()) {
        return -1;
    }
    const QString targetSpeaker = speakerId.trimmed();
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
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
        return -1;
    }
    return qMax<int64_t>(0, static_cast<int64_t>(std::floor(earliestStartSeconds * kTimelineFps)));
}


bool SpeakersTab::ensureAiActionReady(const QString& actionTitle) const
{
    if (!m_speakerDeps.ensureAiSession) {
        return true;
    }
    QString error;
    if (m_speakerDeps.ensureAiSession(&error)) {
        return true;
    }
    QMessageBox::warning(
        nullptr,
        actionTitle,
        error.isEmpty()
            ? QStringLiteral("AI login required. Use top-right Log In.")
            : error);
    return false;
}

void SpeakersTab::updateSpeakerTrackingStatusLabel()
{
    if (!m_widgets.speakerTrackingStatusLabel) {
        return;
    }
    updateSpeakerFramingTargetControls();

    const bool mutableCut = activeCutMutable();
    const QString speakerId = selectedSpeakerId();
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool hasClip = selectedClip != nullptr;
    const bool hasTranscript = m_transcriptSession.hasObjectDocument();
    const bool canRunClipActions = mutableCut && hasClip && hasTranscript;
    const bool canRunTranscriptActions = mutableCut && hasTranscript;
    const bool canEditSpeakerActions = mutableCut && !speakerId.isEmpty();
    const bool canEditClipFraming = mutableCut && m_speakerDeps.updateClipById && selectedClip;
    const bool hasPlayheadTrackSelection =
        m_widgets.speakerPlayheadFaceDetectionsList &&
        !m_widgets.speakerPlayheadFaceDetectionsList->selectedItems().isEmpty();
    if (m_widgets.speakerRefsChipLabel) {
        m_widgets.speakerRefsChipLabel->setText(QStringLiteral("Assigned Tracks: 0"));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        m_widgets.speakerPointstreamChipLabel->setText(QStringLiteral("Continuity Tracks: None"));
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setText(QStringLiteral("Speaker Tracking: OFF"));
        m_widgets.speakerTrackingChipButton->setChecked(false);
        m_widgets.speakerTrackingChipButton->setEnabled(false);
    }
    if (m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setText(QStringLiteral("Face Stabilize: OFF"));
        m_widgets.speakerStabilizeChipButton->setChecked(false);
        m_widgets.speakerStabilizeChipButton->setEnabled(false);
    }

    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setEnabled(canRunClipActions);
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Generate Continuity Tracks"));
    }
    if (m_widgets.speakerViewFacestreamButton) {
        m_widgets.speakerViewFacestreamButton->setEnabled(hasClip && hasTranscript);
    }
    if (m_widgets.speakerFacestreamSettingsButton) {
        m_widgets.speakerFacestreamSettingsButton->setEnabled(canRunClipActions);
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setEnabled(true);
    }
    if (m_widgets.speakerPrecropFacesButton) {
        m_widgets.speakerPrecropFacesButton->setEnabled(
            canEditSpeakerActions && !speakerId.isEmpty() && hasPlayheadTrackSelection);
    }
    if (m_widgets.speakerAiFindNamesButton) {
        m_widgets.speakerAiFindNamesButton->setEnabled(canRunTranscriptActions);
    }
    if (m_widgets.speakerAiFindOrganizationsButton) {
        m_widgets.speakerAiFindOrganizationsButton->setEnabled(canRunTranscriptActions);
    }
    if (m_widgets.speakerAiCleanAssignmentsButton) {
        m_widgets.speakerAiCleanAssignmentsButton->setEnabled(canRunTranscriptActions);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        m_widgets.selectedSpeakerPreviousSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        m_widgets.selectedSpeakerNextSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerNextSectionButton) {
        m_widgets.selectedSpeakerNextSectionButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerRandomSentenceButton) {
        m_widgets.selectedSpeakerRandomSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    const bool canEditFramingTargets = canEditClipFraming;
    if (m_widgets.speakerFramingTargetXSpin) {
        m_widgets.speakerFramingTargetXSpin->setEnabled(canEditFramingTargets);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        m_widgets.speakerFramingTargetYSpin->setEnabled(canEditFramingTargets);
    }
    if (m_widgets.speakerFramingTargetBoxSpin) {
        const bool zoomEnabled =
            m_widgets.speakerFramingZoomEnabledCheckBox &&
            m_widgets.speakerFramingZoomEnabledCheckBox->isChecked();
        m_widgets.speakerFramingTargetBoxSpin->setEnabled(canEditFramingTargets && zoomEnabled);
    }
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        m_widgets.speakerFramingZoomEnabledCheckBox->setEnabled(canEditFramingTargets);
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        m_widgets.speakerApplyFramingToClipCheckBox->setEnabled(false);
    }
    if (m_widgets.speakerFramingEnabledKeyframeTable) {
        m_widgets.speakerFramingEnabledKeyframeTable->setEnabled(canEditClipFraming);
    }

    const bool hasContinuityArtifact =
        selectedClip && selectedClipHasFaceDetectionsSidecars();
    const QJsonArray streams = selectedClip ? continuityStreamsForClip(*selectedClip) : QJsonArray{};
    const bool hasContinuityTracks = !streams.isEmpty();
    const ContinuityCoverageSummary coverage =
        selectedClip
            ? continuityCoverageSummaryForClip(m_transcriptSession.transcriptPath(), selectedClip->id)
            : ContinuityCoverageSummary{};

    m_widgets.speakerTrackingStatusLabel->setToolTip(QString());

    if (!hasClip) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Select a clip to inspect speaker identities, continuity tracks, and raw detections."));
        return;
    }
    if (!hasTranscript) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Transcript not loaded for the selected clip."));
        return;
    }
    if (!mutableCut) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Original cut is read-only. Switch to a derived cut to generate tracks, assign speakers, or edit framing."));
        return;
    }
    if (speakerId.isEmpty()) {
        if (!hasContinuityArtifact) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Continuity tracks have not been generated for this clip yet."));
        } else if (!hasContinuityTracks) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Continuity artifact exists, but no usable tracks were imported for this clip."));
        } else {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("Select a speaker to review assignments. Playhead tracks remain available for inspection and assignment."));
        }
        return;
    }

    const QJsonObject profiles =
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    const int assignedFaceDetectionsCount =
        selectedClip ? resolvedAssignedTrackIdsForSpeaker(*selectedClip, streams, speakerId).size() : 0;
    if (m_widgets.speakerRefsChipLabel) {
        m_widgets.speakerRefsChipLabel->setText(
            QStringLiteral("Assigned Tracks: %1").arg(assignedFaceDetectionsCount));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        if (!hasContinuityArtifact) {
            m_widgets.speakerPointstreamChipLabel->setText(
                QStringLiteral("Continuity Tracks: MISSING (Clip)"));
        } else if (!hasContinuityTracks) {
            m_widgets.speakerPointstreamChipLabel->setText(
                QStringLiteral("Continuity Tracks: Ready (0 tracks)"));
        } else {
            m_widgets.speakerPointstreamChipLabel->setText(
                QStringLiteral("Continuity Tracks: Ready (Clip)"));
        }
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setText(
            trackingEnabled ? QStringLiteral("Speaker Tracking: ON")
                            : QStringLiteral("Speaker Tracking: OFF"));
        m_widgets.speakerTrackingChipButton->setChecked(trackingEnabled);
        m_widgets.speakerTrackingChipButton->setEnabled(canEditSpeakerActions && hasContinuityArtifact);
    }
    QString clipFramingState = QStringLiteral("OFF");
    if (selectedClip) {
        const int64_t timelineFrame =
            m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : selectedClip->startFrame;
        const bool framingEnabledAtFrame =
            evaluateClipSpeakerFramingEnabledAtFrame(*selectedClip, timelineFrame);
        const TimelineClip::TransformKeyframe framingTarget =
            evaluateClipSpeakerFramingTargetAtFrame(*selectedClip, timelineFrame);
        const bool hasActiveTargetBox = framingTarget.scaleX > 0.0;
        const int64_t localFrame = qBound<int64_t>(
            0,
            timelineFrame - selectedClip->startFrame,
            qMax<int64_t>(0, selectedClip->durationFrames - 1));
        const int64_t sourceFrame = qMax<int64_t>(0, selectedClip->sourceInFrame + localFrame);
        QString activeTranscriptSpeaker;
        QPointF sampleLocation;
        qreal sampleBox = -1.0;
        const bool hasRuntimeSample = transcriptActiveSpeakerTrackingSampleForClipFileAtSourceFrame(
            selectedClip->filePath,
            sourceFrame,
            selectedClip->speakerFramingMinConfidence,
            &sampleLocation,
            &sampleBox,
            &activeTranscriptSpeaker) && sampleBox > 0.0;
        const bool hasTranscriptSpeaker = !activeTranscriptSpeaker.isEmpty();
        const bool framingCanRun =
            !selectedClip->speakerFramingKeyframes.isEmpty() ||
            (hasRuntimeSample && hasActiveTargetBox);
        const QString inactiveReason = !framingEnabledAtFrame
            ? QString()
            : (!hasTranscriptSpeaker && selectedClip->speakerFramingKeyframes.isEmpty()
                   ? QStringLiteral("inactive: no transcript speaker")
                   : (!hasActiveTargetBox && selectedClip->speakerFramingKeyframes.isEmpty()
                          ? QStringLiteral("inactive: target box off")
                          : (!hasRuntimeSample && selectedClip->speakerFramingKeyframes.isEmpty()
                                 ? QStringLiteral("inactive: no FaceDetections sample")
                                 : QString())));
        clipFramingState = !framingEnabledAtFrame
            ? QStringLiteral("OFF")
            : (framingCanRun ? QStringLiteral("ACTIVE")
                             : QStringLiteral("ON, %1").arg(inactiveReason));
        if (m_widgets.speakerClipFramingStatusLabel) {
            m_widgets.speakerClipFramingStatusLabel->setText(
                QStringLiteral("Face Stabilize: %1 | Enable: %2 keys | %3")
                    .arg(clipFramingState)
                    .arg(selectedClip->speakerFramingEnabledKeyframes.size())
                    .arg(hasTranscriptSpeaker
                        ? QStringLiteral("Transcript speaker %1").arg(activeTranscriptSpeaker)
                        : QStringLiteral("%1 keys").arg(selectedClip->speakerFramingKeyframes.size())));
        }
        if (m_widgets.speakerStabilizeChipButton) {
            const bool canToggleStabilize = canEditFramingTargets;
            m_widgets.speakerStabilizeChipButton->setText(
                !framingEnabledAtFrame
                    ? QStringLiteral("Face Stabilize: OFF")
                    : (framingCanRun ? QStringLiteral("Face Stabilize: ACTIVE")
                                     : QStringLiteral("Face Stabilize: INACTIVE")));
            m_widgets.speakerStabilizeChipButton->setChecked(framingEnabledAtFrame);
            m_widgets.speakerStabilizeChipButton->setEnabled(canToggleStabilize);
            if (framingEnabledAtFrame && !framingCanRun) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    inactiveReason.isEmpty()
                        ? QStringLiteral("Face Stabilize is enabled but cannot currently produce a transform.")
                        : QStringLiteral("Face Stabilize is enabled but %1.").arg(inactiveReason));
            } else if (!framingEnabledAtFrame) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize follows the active speaker from the transcript."));
            } else if (hasTranscriptSpeaker) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize follows transcript speaker %1 at this frame.")
                        .arg(activeTranscriptSpeaker));
            } else {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize follows the active speaker from the transcript."));
            }
        }
        if (m_widgets.speakerApplyFramingToClipCheckBox) {
            const bool canToggleStabilizeFromCheckbox =
                canEditFramingTargets;
            m_widgets.speakerApplyFramingToClipCheckBox->setEnabled(canToggleStabilizeFromCheckbox);
        }
    }
    if (!selectedClip && m_widgets.speakerClipFramingStatusLabel) {
        m_widgets.speakerClipFramingStatusLabel->setText(QStringLiteral("Face Stabilize: OFF | 0 keys"));
    }
    if (!selectedClip && m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setText(QStringLiteral("Face Stabilize: OFF"));
        m_widgets.speakerStabilizeChipButton->setChecked(false);
        m_widgets.speakerStabilizeChipButton->setEnabled(false);
    }

    if (!hasContinuityArtifact) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Continuity tracks have not been generated for this clip yet. Use Generate Continuity Tracks to create them."));
        return;
    }
    if (!hasContinuityTracks) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Continuity artifact exists, but no processed continuity tracks are available for this clip."));
        return;
    }

    QString status =
        QStringLiteral("Speaker: %1 | Assigned Tracks: %2 | Continuity Tracks: %3 | Speaker Tracking: %4 | Face Stabilize: %5")
            .arg(speakerDisplayLabel(speakerId))
            .arg(assignedFaceDetectionsCount)
            .arg(streams.size())
            .arg(trackingEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(selectedClip ? clipFramingState : QStringLiteral("OFF"));
    const bool showPlayheadTracks =
        !m_widgets.speakerShowPlayheadFaceDetectionsCheckBox ||
        m_widgets.speakerShowPlayheadFaceDetectionsCheckBox->isChecked();
    status += QStringLiteral("\nTracks At Playhead: %1 | Visible matches: %2")
                  .arg(showPlayheadTracks ? QStringLiteral("VISIBLE") : QStringLiteral("HIDDEN"))
                  .arg(m_lastPlayheadTrackCandidateCount);
    QString tooltip =
        QStringLiteral("UI timings ms | Speakers: %1 (max %2) | Playhead: %3 (max %4) | Tracks: %5 (max %6) | Detections: %7 (max %8)")
            .arg(m_lastSpeakersTableRefreshDurationMs)
            .arg(m_maxSpeakersTableRefreshDurationMs)
            .arg(m_lastPlayheadTrackCandidatesRefreshDurationMs)
            .arg(m_maxPlayheadTrackCandidatesRefreshDurationMs)
            .arg(m_lastFaceDetectionsPanelRefreshDurationMs)
            .arg(m_maxFaceDetectionsPanelRefreshDurationMs)
            .arg(m_lastRawDetectionsPanelRefreshDurationMs)
            .arg(m_maxRawDetectionsPanelRefreshDurationMs);
    if (selectedClip && coverage.hasRawTrackCoverage) {
        if (selectedClip->sourceInFrame < coverage.minRawTrackFrame ||
            selectedClip->sourceInFrame > coverage.maxRawTrackFrame) {
            status += QStringLiteral(
                "\n[WARN] Imported continuity artifact only covers source frames %1..%2, while this clip starts at source frame %3.")
                          .arg(coverage.minRawTrackFrame)
                          .arg(coverage.maxRawTrackFrame)
                          .arg(selectedClip->sourceInFrame);
        } else {
            status += QStringLiteral("\nArtifact coverage: source frames %1..%2 | raw tracks: %3")
                          .arg(coverage.minRawTrackFrame)
                          .arg(coverage.maxRawTrackFrame)
                          .arg(coverage.rawTrackCount);
        }
    }
    if (!coverage.runId.isEmpty()) {
        status += QStringLiteral("\nArtifact run: %1").arg(coverage.runId);
    }
    if (!coverage.importedArtifactDir.isEmpty()) {
        status += QStringLiteral("\nImported from: %1").arg(coverage.importedArtifactDir);
    }
    m_widgets.speakerTrackingStatusLabel->setText(status);
    m_widgets.speakerTrackingStatusLabel->setToolTip(tooltip);
}

void SpeakersTab::updateSpeakerTrackingStatusLabelFast()
{
    if (!m_widgets.speakerTrackingStatusLabel) {
        return;
    }

    updateSpeakerFramingTargetControls();

    const bool mutableCut = activeCutMutable();
    const QString speakerId = selectedSpeakerId();
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool hasClip = selectedClip != nullptr;
    const bool hasTranscript = m_transcriptSession.hasObjectDocument();
    const bool canRunClipActions = mutableCut && hasClip && hasTranscript;
    const bool canRunTranscriptActions = mutableCut && hasTranscript;
    const bool canEditSpeakerActions = mutableCut && !speakerId.isEmpty();
    const bool canEditClipFraming = mutableCut && m_speakerDeps.updateClipById && selectedClip;
    const bool hasPlayheadTrackSelection =
        m_widgets.speakerPlayheadFaceDetectionsList &&
        !m_widgets.speakerPlayheadFaceDetectionsList->selectedItems().isEmpty();

    if (m_widgets.speakerRefsChipLabel) {
        m_widgets.speakerRefsChipLabel->setText(QStringLiteral("Assigned Tracks: refreshing"));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        m_widgets.speakerPointstreamChipLabel->setText(QStringLiteral("Continuity Tracks: refreshing"));
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setEnabled(false);
    }
    if (m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setEnabled(false);
    }
    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setEnabled(canRunClipActions);
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Generate Continuity Tracks"));
    }
    if (m_widgets.speakerViewFacestreamButton) {
        m_widgets.speakerViewFacestreamButton->setEnabled(hasClip && hasTranscript);
    }
    if (m_widgets.speakerFacestreamSettingsButton) {
        m_widgets.speakerFacestreamSettingsButton->setEnabled(canRunClipActions);
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setEnabled(true);
    }
    if (m_widgets.speakerPrecropFacesButton) {
        m_widgets.speakerPrecropFacesButton->setEnabled(
            canEditSpeakerActions && hasPlayheadTrackSelection);
    }
    if (m_widgets.speakerAiFindNamesButton) {
        m_widgets.speakerAiFindNamesButton->setEnabled(canRunTranscriptActions);
    }
    if (m_widgets.speakerAiFindOrganizationsButton) {
        m_widgets.speakerAiFindOrganizationsButton->setEnabled(canRunTranscriptActions);
    }
    if (m_widgets.speakerAiCleanAssignmentsButton) {
        m_widgets.speakerAiCleanAssignmentsButton->setEnabled(canRunTranscriptActions);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        m_widgets.selectedSpeakerPreviousSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        m_widgets.selectedSpeakerNextSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerNextSectionButton) {
        m_widgets.selectedSpeakerNextSectionButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerRandomSentenceButton) {
        m_widgets.selectedSpeakerRandomSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.speakerFramingTargetXSpin) {
        m_widgets.speakerFramingTargetXSpin->setEnabled(canEditClipFraming);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        m_widgets.speakerFramingTargetYSpin->setEnabled(canEditClipFraming);
    }
    if (m_widgets.speakerFramingTargetBoxSpin) {
        const bool zoomEnabled =
            m_widgets.speakerFramingZoomEnabledCheckBox &&
            m_widgets.speakerFramingZoomEnabledCheckBox->isChecked();
        m_widgets.speakerFramingTargetBoxSpin->setEnabled(canEditClipFraming && zoomEnabled);
    }
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        m_widgets.speakerFramingZoomEnabledCheckBox->setEnabled(canEditClipFraming);
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        m_widgets.speakerApplyFramingToClipCheckBox->setEnabled(false);
    }
    if (m_widgets.speakerFramingEnabledKeyframeTable) {
        m_widgets.speakerFramingEnabledKeyframeTable->setEnabled(canEditClipFraming);
    }

    m_widgets.speakerTrackingStatusLabel->setToolTip(QString());
    if (!hasClip) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Select a clip to inspect speaker identities, continuity tracks, and raw detections."));
    } else if (!hasTranscript) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Transcript not loaded for the selected clip."));
    } else if (!mutableCut) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Original cut is read-only. Switch to a derived cut to generate tracks, assign speakers, or edit framing."));
    } else if (speakerId.isEmpty()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Select a speaker to review assignments. Track details refreshing."));
    } else {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Speaker: %1 | Track details refreshing")
                .arg(speakerDisplayLabel(speakerId)));
    }
}
