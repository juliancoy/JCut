#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speakers_table.h"

#include "facestream_runtime.h"
#include "facestream_time_mapping.h"
#include "facestream_artifact_utils.h"
#include "decoder_context.h"
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

SpeakersTab::SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : TableTabBase(deps, parent)
    , m_widgets(widgets)
    , m_speakerDeps(deps)
{
    connect(&m_transcriptLoadWatcher, &QFutureWatcher<TranscriptDocumentLoadResult>::finished, this, [this]() {
        const TranscriptDocumentLoadResult result = m_transcriptLoadWatcher.result();
        if (!result.ok) {
            if (result.transcriptPath == m_loadedTranscriptPath &&
                result.clipFilePath == m_loadedClipFilePath &&
                m_widgets.speakersInspectorDetailsLabel) {
                m_widgets.speakersInspectorDetailsLabel->setText(
                    result.error.isEmpty() ? QStringLiteral("Unable to load transcript JSON file.") : result.error);
            }
            m_updating = false;
            updateSelectedSpeakerPanel();
            updateSpeakerTrackingStatusLabel();
            return;
        }
        if (result.transcriptPath != m_loadedTranscriptPath || result.clipFilePath != m_loadedClipFilePath) {
            return;
        }
        m_loadedTranscriptDoc = result.document;
        const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        if (!clip || clip->filePath != result.clipFilePath) {
            return;
        }
        applyLoadedTranscriptDocumentData(*clip, m_pendingPreferredSpeakerId);
    });
    connect(&m_transcriptSaveWatcher, &QFutureWatcher<TranscriptDocumentSaveResult>::finished, this, [this]() {
        const TranscriptDocumentSaveResult result = m_transcriptSaveWatcher.result();
        if (!result.ok) {
            qWarning().noquote()
                << QStringLiteral("[speakers] async save failed for %1: %2")
                       .arg(result.transcriptPath,
                            result.error.isEmpty() ? QStringLiteral("unknown error") : result.error);
        }
        if (m_pendingTranscriptSaveRevision > result.revision &&
            !m_pendingTranscriptSavePath.trimmed().isEmpty() &&
            m_pendingTranscriptSaveDoc.isObject()) {
            const QString path = m_pendingTranscriptSavePath;
            const QJsonDocument doc = m_pendingTranscriptSaveDoc;
            const qint64 revision = m_pendingTranscriptSaveRevision;
            m_transcriptSaveWatcher.setFuture(QtConcurrent::run([path, doc, revision]() {
                return saveTranscriptDocumentResult(path, doc, revision);
            }));
        }
    });
}

bool SpeakersTab::updateLoadedTranscriptDocument(const std::function<bool(QJsonObject&)>& mutator)
{
    if (!m_loadedTranscriptDoc.isObject() || !mutator) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    if (!mutator(root)) {
        return false;
    }

    m_loadedTranscriptDoc.setObject(root);
    clearFaceStreamDerivedCaches();
    return true;
}

bool SpeakersTab::saveLoadedTranscriptDocument()
{
    if (m_loadedTranscriptPath.trimmed().isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    queueLoadedTranscriptDocumentSave();
    return true;
}

void SpeakersTab::queueLoadedTranscriptDocumentSave()
{
    if (m_loadedTranscriptPath.trimmed().isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    ++m_transcriptSaveRevision;
    m_pendingTranscriptSaveRevision = m_transcriptSaveRevision;
    m_pendingTranscriptSavePath = m_loadedTranscriptPath;
    m_pendingTranscriptSaveDoc = m_loadedTranscriptDoc;
    if (shouldUseSynchronousTranscriptIo()) {
        editor::TranscriptEngine engine;
        engine.saveTranscriptJson(m_pendingTranscriptSavePath, m_pendingTranscriptSaveDoc);
        return;
    }
    if (m_transcriptSaveWatcher.isRunning()) {
        return;
    }
    const QString path = m_pendingTranscriptSavePath;
    const QJsonDocument doc = m_pendingTranscriptSaveDoc;
    const qint64 revision = m_pendingTranscriptSaveRevision;
    m_transcriptSaveWatcher.setFuture(QtConcurrent::run([path, doc, revision]() {
        return saveTranscriptDocumentResult(path, doc, revision);
    }));
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
            m_loadedTranscriptDoc = result.document;
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
        m_widgets.speakersInspectorDetailsLabel->setText(
            QStringLiteral("FaceStream sidecar: %1")
                .arg(facestreamSidecarExistsForClipFile(clip.filePath)
                         ? QStringLiteral("Present")
                         : QStringLiteral("Missing")));
    }

    refreshSpeakersTable(m_loadedTranscriptDoc.object(), preferredSpeakerId);
    refreshSpeakerSectionsTable(m_loadedTranscriptDoc.object());
    requestRefreshFaceStreamPathsPanel();
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }

    m_updating = false;
    updateSelectedSpeakerPanel();
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::clearFaceStreamDerivedCaches()
{
    m_avatarCache.clear();
    m_avatarHoverTooltipHtmlCache.clear();
    m_continuityStreamsCache.clear();
    m_speakersTableRefreshSignature.clear();
}

bool SpeakersTab::clipSupportsTranscript(const TimelineClip& clip) const
{
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}


bool SpeakersTab::assignTrackToSpeaker(const QString& speakerId,
                                       int trackId,
                                       const QString& streamId,
                                       int64_t sourceFrame,
                                       qreal xNorm,
                                       qreal yNorm,
                                       qreal boxSizeNorm,
                                       const QString& resolutionSource)
{
    if (!activeCutMutable() || trackId < 0 || speakerId.trimmed().isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
    QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    QJsonObject clipRoot = clipsRoot.value(clip->id).toObject();
    clipRoot[QStringLiteral("clip_id")] = clip->id;
    clipRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonObject resolvedPayload = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    resolvedPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QJsonArray resolvedMap = resolvedPayload.value(QStringLiteral("track_identity_map")).toArray();
    QJsonArray nextResolvedMap;
    bool replaced = false;
    for (const QJsonValue& value : resolvedMap) {
        QJsonObject row = value.toObject();
        const int rowTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
        if (rowTrackId == trackId) {
            row[QStringLiteral("identity_id")] = speakerId;
            row[QStringLiteral("stream_id")] = streamId;
            row[QString(kSpeakerFlowAnchorSourceFrameKey)] = static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
            row[QString(kSpeakerFlowAnchorXKey)] = qBound<qreal>(0.0, xNorm, 1.0);
            row[QString(kSpeakerFlowAnchorYKey)] = qBound<qreal>(0.0, yNorm, 1.0);
            row[QString(kSpeakerFlowAnchorBoxSizeKey)] = qBound<qreal>(0.01, boxSizeNorm, 1.0);
            row[QStringLiteral("resolution_source")] = resolutionSource.trimmed().isEmpty()
                ? QStringLiteral("speaker_track_picker")
                : resolutionSource.trimmed();
            row[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            replaced = true;
        }
        nextResolvedMap.push_back(row);
    }
    if (!replaced) {
        QJsonObject row;
        row[QStringLiteral("track_id")] = trackId;
        row[QStringLiteral("identity_id")] = speakerId;
        row[QStringLiteral("stream_id")] = streamId;
        row[QString(kSpeakerFlowAnchorSourceFrameKey)] = static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
        row[QString(kSpeakerFlowAnchorXKey)] = qBound<qreal>(0.0, xNorm, 1.0);
        row[QString(kSpeakerFlowAnchorYKey)] = qBound<qreal>(0.0, yNorm, 1.0);
        row[QString(kSpeakerFlowAnchorBoxSizeKey)] = qBound<qreal>(0.01, boxSizeNorm, 1.0);
        row[QStringLiteral("resolution_source")] = resolutionSource.trimmed().isEmpty()
            ? QStringLiteral("speaker_track_picker")
            : resolutionSource.trimmed();
        row[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        nextResolvedMap.push_back(row);
    }
    resolvedPayload[QStringLiteral("track_identity_map")] = nextResolvedMap;
    clipRoot[QStringLiteral("resolved_current")] = resolvedPayload;

    QJsonObject humanRuns = clipRoot.value(QStringLiteral("human_runs")).toObject();
    const QString runId = QStringLiteral("track_picker_%1").arg(
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")));
    QJsonObject humanPayload;
    humanPayload[QStringLiteral("run_id")] = runId;
    humanPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonArray auditLog;
    QJsonObject auditRow;
    auditRow[QStringLiteral("timestamp_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    auditRow[QStringLiteral("action")] = QStringLiteral("speaker_track_picker_identity_set");
    auditRow[QStringLiteral("track_id")] = trackId;
    auditRow[QStringLiteral("stream_id")] = streamId;
    auditRow[QStringLiteral("identity_id")] = speakerId;
    auditRow[QString(kSpeakerFlowAnchorSourceFrameKey)] = static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
    auditRow[QString(kSpeakerFlowAnchorXKey)] = qBound<qreal>(0.0, xNorm, 1.0);
    auditRow[QString(kSpeakerFlowAnchorYKey)] = qBound<qreal>(0.0, yNorm, 1.0);
    auditRow[QString(kSpeakerFlowAnchorBoxSizeKey)] = qBound<qreal>(0.01, boxSizeNorm, 1.0);
    auditLog.push_back(auditRow);
    humanPayload[QStringLiteral("audit_log")] = auditLog;
    humanRuns[runId] = humanPayload;
    clipRoot[QStringLiteral("human_runs")] = humanRuns;
    clipRoot[QStringLiteral("latest_human_run_id")] = runId;

    QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonArray faceRefs = speakerFaceRefs(profile);
    bool faceRefExists = false;
    for (const QJsonValue& faceRefValue : faceRefs) {
        const QJsonObject faceRef = faceRefValue.toObject();
        if (faceRef.value(QStringLiteral("track_id")).toInt(-1) == trackId) {
            faceRefExists = true;
            break;
        }
    }
    if (!faceRefExists) {
        QJsonObject faceRef;
        faceRef[QStringLiteral("track_id")] = trackId;
        faceRef[QStringLiteral("stream_id")] = streamId;
        faceRef[QString(kSpeakerFlowAnchorSourceFrameKey)] =
            static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
        faceRef[QString(kSpeakerFlowAnchorXKey)] = qBound<qreal>(0.0, xNorm, 1.0);
        faceRef[QString(kSpeakerFlowAnchorYKey)] = qBound<qreal>(0.0, yNorm, 1.0);
        faceRef[QString(kSpeakerFlowAnchorBoxSizeKey)] = qBound<qreal>(0.01, boxSizeNorm, 1.0);
        faceRef[QStringLiteral("source")] = resolutionSource.trimmed().isEmpty()
            ? QStringLiteral("speaker_track_picker")
            : resolutionSource.trimmed();
        faceRefs.push_back(faceRef);
        profile[QString(kTranscriptSpeakerFaceRefsKey)] = faceRefs;
        profiles[speakerId] = profile;
        transcriptRoot[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    }

    clipsRoot[clip->id] = clipRoot;
    speakerFlow[QStringLiteral("clips")] = clipsRoot;
    transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
    if (!updateLoadedTranscriptDocument([&](QJsonObject& root) {
            root = transcriptRoot;
            return true;
        }) || !saveLoadedTranscriptDocument()) {
        refresh();
        return false;
    }
    m_avatarHoverTooltipHtmlCache.clear();

    QJsonObject identityRoot;
    engine.loadIdentityArtifact(m_loadedTranscriptPath, &identityRoot);
    QJsonObject assignmentsByClip = identityRoot.value(QStringLiteral("identity_assignments_by_clip")).toObject();
    QJsonObject assignmentRoot = assignmentsByClip.value(clip->id).toObject();
    assignmentRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    assignmentRoot[QStringLiteral("track_identity_map")] = nextResolvedMap;
    assignmentsByClip[clip->id] = assignmentRoot;
    identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
    identityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    identityRoot[QStringLiteral("identity_assignments_by_clip")] = assignmentsByClip;
    engine.saveIdentityArtifact(m_loadedTranscriptPath, identityRoot);

    m_faceStreamPanelRefreshSignature.clear();
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

void SpeakersTab::openTrackPickerForSpeaker(const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (trimmedSpeakerId.isEmpty() || !clip || !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    const QJsonArray streams = continuityStreamsForClip(*clip);
    if (streams.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("No continuity tracks are available for this clip."));
        return;
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Add Tracks to %1").arg(speakerDisplayLabel(trimmedSpeakerId)));
    dialog.resize(960, 640);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(
        QStringLiteral("Select one or more continuity tracks. Each thumbnail uses the midpoint frame of that track."),
        &dialog);
    help->setWordWrap(true);
    auto* list = new QListWidget(&dialog);
    list->setViewMode(QListView::IconMode);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setIconSize(QSize(96, 96));
    list->setSpacing(10);

    const QString mediaPath = interactivePreviewMediaPathForClip(*clip);
    std::unique_ptr<editor::DecoderContext> decoder;
    if (!mediaPath.isEmpty()) {
        decoder = std::make_unique<editor::DecoderContext>(mediaPath);
        if (!decoder->initialize()) {
            decoder.reset();
        }
    }
    QHash<int64_t, QImage> frameImageCache;
    const qreal sourceFps = resolvedSourceFps(*clip);
    const QHash<int, QString> identityByTrackId = resolvedIdentityByTrackId(*clip, streams);
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString(
            QStringLiteral("T%1").arg(trackId));
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (trackId < 0 || keyframes.isEmpty()) {
            continue;
        }
        const QJsonObject midpointKeyframe = keyframes.at(keyframes.size() / 2).toObject();
        if (midpointKeyframe.isEmpty()) {
            continue;
        }
        const QString assignedSpeakerId = identityByTrackId.value(trackId).trimmed();
        const QString assignedLabel =
            assignedSpeakerId.isEmpty() ? QStringLiteral("Unassigned") : speakerDisplayLabel(assignedSpeakerId);
        auto* item = new QListWidgetItem(
            QIcon(faceStreamPreviewAvatarWithDecoder(
                *clip,
                trimmedSpeakerId,
                midpointKeyframe,
                96,
                decoder.get(),
                &frameImageCache,
                sourceFps)),
            QStringLiteral("%1\n%2").arg(streamId, assignedLabel));
        item->setData(Qt::UserRole, trackId);
        item->setData(Qt::UserRole + 1, streamId);
        item->setData(Qt::UserRole + 2, QString::fromUtf8(QJsonDocument(midpointKeyframe).toJson(QJsonDocument::Compact)));
        item->setToolTip(
            QStringLiteral("Track %1\nCurrent assignment: %2").arg(trackId).arg(assignedLabel));
        item->setSizeHint(QSize(120, 130));
        if (assignedSpeakerId == trimmedSpeakerId) {
            item->setSelected(true);
        }
        list->addItem(item);
    }

    auto* buttonsRow = new QHBoxLayout;
    auto* addButton = new QPushButton(QStringLiteral("Assign Selected"), &dialog);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    buttonsRow->addStretch(1);
    buttonsRow->addWidget(addButton);
    buttonsRow->addWidget(cancelButton);
    layout->addWidget(help);
    layout->addWidget(list, 1);
    layout->addLayout(buttonsRow);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(addButton, &QPushButton::clicked, &dialog, [&dialog]() { dialog.accept(); });

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    bool changed = false;
    for (QListWidgetItem* item : list->selectedItems()) {
        const int trackId = item->data(Qt::UserRole).toInt();
        const QString streamId = item->data(Qt::UserRole + 1).toString().trimmed();
        const QJsonObject keyframe =
            QJsonDocument::fromJson(item->data(Qt::UserRole + 2).toString().toUtf8()).object();
        if (trackId < 0 || keyframe.isEmpty()) {
            continue;
        }
        const int64_t sourceFrame =
            qMax<int64_t>(0, keyframe.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
        const qreal xNorm =
            qBound<qreal>(0.0, keyframe.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const qreal yNorm =
            qBound<qreal>(0.0, keyframe.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
        const qreal boxNorm =
            qBound<qreal>(0.01, keyframe.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2), 1.0);
        changed = assignTrackToSpeaker(
                      trimmedSpeakerId,
                      trackId,
                      streamId.isEmpty() ? QStringLiteral("T%1").arg(trackId) : streamId,
                      sourceFrame,
                      xNorm,
                      yNorm,
                      boxNorm,
                      QStringLiteral("speaker_track_picker")) || changed;
    }
    if (!changed) {
        updateSpeakerTrackingStatusLabel();
        updateSelectedSpeakerPanel();
    }
}

bool SpeakersTab::rebuildProcessedFaceStreamForSelectedClip(bool interactive)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        if (interactive) {
            QMessageBox::information(nullptr,
                                     QStringLiteral("Rebuild Processed FaceStream"),
                                     QStringLiteral("Select a clip first."));
        }
        return false;
    }
    if (m_loadedTranscriptPath.trimmed().isEmpty()) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceStream"),
                                 QStringLiteral("No active transcript is loaded for this clip."));
        }
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject rawArtifactRoot;
    if (!engine.loadFacestreamArtifact(m_loadedTranscriptPath, &rawArtifactRoot)) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceStream"),
                                 QStringLiteral("No raw FaceStream artifact was found for this transcript."));
        }
        return false;
    }

    const QString clipId = clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id.trimmed();
    QJsonObject byClip = continuityFacestreamsByClipObject(rawArtifactRoot);
    QJsonObject continuityRoot = byClip.value(clipId).toObject();
    if (continuityRoot.isEmpty()) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceStream"),
                                 QStringLiteral("No raw FaceStream payload was found for the selected clip."));
        }
        return false;
    }

    continuityRoot[QStringLiteral("clip_id")] = clipId;
    continuityRoot[QStringLiteral("processed_artifact_path")] =
        engine.facestreamProcessedArtifactPath(m_loadedTranscriptPath);
    byClip[clipId] = continuityRoot;
    setContinuityFacestreamsByClipObject(&rawArtifactRoot, byClip);
    if (!engine.saveFacestreamArtifact(m_loadedTranscriptPath, rawArtifactRoot)) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceStream"),
                                 QStringLiteral("Failed to update raw FaceStream artifact metadata."));
        }
        return false;
    }

    if (!jcut::facestream::saveProcessedContinuityArtifact(
            m_loadedTranscriptPath,
            clipId,
            continuityRoot,
            m_loadedTranscriptDoc.object(),
            nullptr)) {
        if (interactive) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Rebuild Processed FaceStream"),
                                 QStringLiteral("Failed to rebuild processed FaceStream sidecar."));
        }
        return false;
    }

    requestRefreshFaceStreamPathsPanel();
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
    if (m_loadedTranscriptPath.isEmpty() || m_loadedClipFilePath.isEmpty()) {
        return false;
    }
    return m_loadedTranscriptPath != originalTranscriptPathForClip(m_loadedClipFilePath);
}

void SpeakersTab::refresh()
{
    m_updating = true;
    hideSpeakerAvatarHoverPreview();
    const QString selectedSpeakerBeforeClear = selectedSpeakerId();
    const QString preferredSpeakerId =
        selectedSpeakerBeforeClear.isEmpty() ? m_lastSelectedSpeakerIdHint : selectedSpeakerBeforeClear;
    const QString previousTranscriptPath = m_loadedTranscriptPath;
    const QString previousClipFilePath = m_loadedClipFilePath;
    m_lastSelectionSeekSpeakerId.clear();
    m_lastSelectionSeekClipId.clear();
    m_pendingReferencePick = 0;

    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->clearContents();
        m_widgets.speakersTable->setRowCount(0);
        m_widgets.speakersTable->setEnabled(false);
        m_widgets.speakersTable->setIconSize(QSize(28, 28));
    }
    requestRefreshFaceStreamPathsPanel();

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip)) {
        m_loadedTranscriptPath.clear();
        m_loadedClipFilePath.clear();
        m_loadedTranscriptDoc = QJsonDocument();
        m_speakersTableRefreshSignature.clear();
        if (m_widgets.speakersInspectorClipLabel) {
            m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("No transcript cut selected"));
        }
        if (m_widgets.speakersInspectorDetailsLabel) {
            m_widgets.speakersInspectorDetailsLabel->setText(
                QStringLiteral("Select a transcript cut to name speakers and set on-screen locations."));
        }
        m_updating = false;
        updateSelectedSpeakerPanel();
        updateSpeakerTrackingStatusLabel();
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    const bool transcriptChanged =
        previousTranscriptPath != transcriptPath || previousClipFilePath != clip->filePath;
    if (transcriptChanged) {
        clearFaceStreamDerivedCaches();
        m_faceStreamPanelRefreshSignature.clear();
    }

    const bool canReuseLoadedDoc =
        m_loadedTranscriptDoc.isObject() &&
        m_loadedTranscriptPath == transcriptPath &&
        m_loadedClipFilePath == clip->filePath;
    if (!canReuseLoadedDoc) {
        m_loadedTranscriptPath = transcriptPath;
        m_loadedClipFilePath = clip->filePath;
        m_loadedTranscriptDoc = QJsonDocument();
        startTranscriptLoadRequest(clip->filePath, transcriptPath, preferredSpeakerId);
        return;
    }
    applyLoadedTranscriptDocumentData(*clip, preferredSpeakerId);
}

void SpeakersTab::refreshForSubtab(const QString& subtabName)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip) || !m_loadedTranscriptDoc.isObject() ||
        m_loadedTranscriptPath != activeTranscriptPathForClipFile(clip->filePath) ||
        m_loadedClipFilePath != clip->filePath) {
        refresh();
        return;
    }

    const QString normalized = subtabName.trimmed();
    if (normalized.compare(QStringLiteral("Continuity Tracks"), Qt::CaseInsensitive) == 0) {
        requestRefreshFaceStreamPathsPanel();
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (normalized.compare(QStringLiteral("Debug"), Qt::CaseInsensitive) == 0) {
        updateSpeakerTrackingStatusLabel();
        return;
    }

    if (m_widgets.speakersTable && m_widgets.speakersTable->rowCount() == 0) {
        refreshSpeakersTable(m_loadedTranscriptDoc.object());
    }
    if (m_widgets.speakerShowContiguousSectionsCheckBox &&
        m_widgets.speakerShowContiguousSectionsCheckBox->isChecked() &&
        m_widgets.speakerSectionsTable &&
        m_widgets.speakerSectionsTable->rowCount() == 0) {
        refreshSpeakerSectionsTable(m_loadedTranscriptDoc.object());
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

bool SpeakersTab::generateFaceStreamForSelectedClip()
{
    if (!activeCutMutable()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("JCut DNN FaceStream Generator"),
            QStringLiteral("FaceStream actions are editable only on derived cuts (not Original)."));
        return false;
    }
    onSpeakerRunAutoTrackClicked();
    return true;
}

editor::ActionResult SpeakersTab::deleteFaceStreamForSelectedClipResult(bool confirmDialog,
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
            QStringLiteral("FaceStream actions are editable only on derived cuts (not Original)."),
            [](const QString& message) {
                QMessageBox::information(
                    nullptr,
                    QStringLiteral("Delete FaceStream"),
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
    engine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot);
    QJsonObject continuityByClip = continuityFacestreamsByClipObject(artifactRoot);
    const QString clipId = clip->id.trimmed();
    const QJsonObject continuityRoot = continuityByClip.value(clipId).toObject();
    const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(
        continuityRoot,
        m_loadedTranscriptDoc.object());
    const bool hasStoredPayload =
        jcut::facestream::continuityRootHasStoredPayload(continuityRoot);
    QString facestreamPartPath = continuityRoot.value(QStringLiteral("facestream_part")).toString().trimmed();
    if (facestreamPartPath.isEmpty()) {
        const QString importedArtifactDir =
            continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed();
        if (!importedArtifactDir.isEmpty()) {
            facestreamPartPath =
                QDir(importedArtifactDir).absoluteFilePath(QStringLiteral("facestream.part"));
        }
    }

    if (streams.isEmpty() && !hasStoredPayload) {
        return fail(
            QStringLiteral("no_facestream_paths"),
            QStringLiteral("No FaceStream paths were found for this clip."),
            [](const QString& message) {
                QMessageBox::information(
                    nullptr,
                    QStringLiteral("Delete FaceStream"),
                    message);
            });
    }

    bool deleteFacestreamPart = false;
    if (confirmDialog) {
        QStringList affectedPaths;
        const QString facestreamArtifactPath = engine.facestreamArtifactPath(m_loadedTranscriptPath);
        if (!facestreamArtifactPath.trimmed().isEmpty()) {
            affectedPaths.push_back(facestreamArtifactPath);
        }
        const QString processedArtifactPath = engine.facestreamProcessedArtifactPath(m_loadedTranscriptPath);
        QJsonObject processedArtifactRoot;
        const bool hasProcessedClipPayload =
            engine.loadFacestreamProcessedArtifact(m_loadedTranscriptPath, &processedArtifactRoot) &&
            jcut::facestream::continuityRootHasStoredPayload(
                continuityRootForClip(processedArtifactRoot, clipId));
        if (hasProcessedClipPayload && !processedArtifactPath.trimmed().isEmpty()) {
            affectedPaths.push_back(processedArtifactPath);
        }
        const QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
        const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        const QJsonObject clipFlow = clipsRoot.value(clipId).toObject();
        const bool hasLegacyTranscriptPayload =
            clipFlow.contains(QStringLiteral("continuity_facestreams"));
        if (hasLegacyTranscriptPayload && !m_loadedTranscriptPath.trimmed().isEmpty()) {
            affectedPaths.push_back(m_loadedTranscriptPath);
        }
        affectedPaths.removeDuplicates();

        QDialog confirmationDialog;
        confirmationDialog.setWindowTitle(QStringLiteral("Delete FaceStream"));
        confirmationDialog.setWindowFlag(Qt::Window, true);
        confirmationDialog.resize(620, 280);

        auto* layout = new QVBoxLayout(&confirmationDialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        QString confirmationMessage =
            QStringLiteral("Delete FaceStream data for this clip?\n\n");
        if (!affectedPaths.isEmpty()) {
            confirmationMessage += QStringLiteral("This will remove this clip's FaceStream entries from:\n");
            for (const QString& path : affectedPaths) {
                confirmationMessage += QStringLiteral("- %1\n").arg(path);
            }
            confirmationMessage += QLatin1Char('\n');
        }
        confirmationMessage += QStringLiteral(
            "By default, debug-run artifacts such as facestream.part, tracks.bin, continuity_facestream.bin, and summary.json are not deleted by this action.\n\nThis cannot be undone.");

        auto* label = new QLabel(confirmationMessage, &confirmationDialog);
        label->setWordWrap(true);
        layout->addWidget(label);

        QCheckBox* deletePartCheckbox = nullptr;
        if (!facestreamPartPath.isEmpty()) {
            deletePartCheckbox = new QCheckBox(
                QStringLiteral("Also delete facestream.part checkpoint"), &confirmationDialog);
            deletePartCheckbox->setToolTip(facestreamPartPath);
            layout->addWidget(deletePartCheckbox);

            auto* partPathLabel = new QLabel(
                QStringLiteral("Checkpoint: %1").arg(facestreamPartPath), &confirmationDialog);
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
                QStringLiteral("Delete FaceStream was canceled."));
        }
        deleteFacestreamPart = deletePartCheckbox && deletePartCheckbox->isChecked();
    }

    continuityByClip.remove(clipId);
    setContinuityFacestreamsByClipObject(&artifactRoot, continuityByClip);
    const bool savedArtifact = engine.saveFacestreamArtifact(m_loadedTranscriptPath, artifactRoot);
    if (!savedArtifact) {
        return fail(
            QStringLiteral("save_failed"),
            QStringLiteral("Failed to save FaceStream artifact after deletion."),
            [](const QString& message) {
                QMessageBox::warning(
                    nullptr,
                    QStringLiteral("Delete FaceStream"),
                    message);
            });
    }

    QJsonObject processedArtifactRoot;
    if (engine.loadFacestreamProcessedArtifact(m_loadedTranscriptPath, &processedArtifactRoot)) {
        QJsonObject processedByClip = continuityFacestreamsByClipObject(processedArtifactRoot);
        if (processedByClip.contains(clipId)) {
            processedByClip.remove(clipId);
            setContinuityFacestreamsByClipObject(&processedArtifactRoot, processedByClip);
            engine.saveFacestreamProcessedArtifact(m_loadedTranscriptPath, processedArtifactRoot);
        }
    }

    // Keep legacy transcript-side continuity fallback in sync if present.
    QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
    bool transcriptChanged = false;
    QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    if (clipsRoot.contains(clipId)) {
        QJsonObject clipFlow = clipsRoot.value(clipId).toObject();
        if (clipFlow.contains(QStringLiteral("continuity_facestreams"))) {
            clipFlow.remove(QStringLiteral("continuity_facestreams"));
            clipsRoot[clipId] = clipFlow;
            speakerFlow[QStringLiteral("clips")] = clipsRoot;
            transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
            transcriptChanged = true;
        }
    }
    if (transcriptChanged) {
        updateLoadedTranscriptDocument([&](QJsonObject& root) {
            root = transcriptRoot;
            return true;
        });
        saveLoadedTranscriptDocument();
    }

    if (deleteFacestreamPart && !facestreamPartPath.isEmpty() && QFileInfo::exists(facestreamPartPath)) {
        if (!QFile::remove(facestreamPartPath)) {
            return fail(
                QStringLiteral("delete_checkpoint_failed"),
                QStringLiteral("FaceStream entries were removed, but deleting facestream.part failed:\n%1")
                    .arg(facestreamPartPath),
                [](const QString& message) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("Delete FaceStream"),
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

bool SpeakersTab::deleteFaceStreamForSelectedClip(bool confirmDialog, QString* errorOut)
{
    const editor::ActionResult result =
        deleteFaceStreamForSelectedClipResult(confirmDialog, true);
    if (!result.ok && errorOut) {
        *errorOut = result.message;
    }
    return result.ok;
}

bool SpeakersTab::selectedClipHasFaceStreamSidecars() const
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || m_loadedTranscriptPath.trimmed().isEmpty()) {
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip->id);
        if (jcut::facestream::continuityRootHasStoredPayload(continuityRoot)) {
            return true;
        }
    }

    QJsonObject processedArtifactRoot;
    if (engine.loadFacestreamProcessedArtifact(m_loadedTranscriptPath, &processedArtifactRoot)) {
        if (jcut::facestream::continuityRootHasStoredPayload(
                continuityRootForClip(processedArtifactRoot, clip->id))) {
            return true;
        }
    }

    const QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
    const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip->id.trimmed()).toObject();
    return !clipFlow.value(QStringLiteral("continuity_facestreams")).toArray().isEmpty();
}

void SpeakersTab::onSpeakerViewFaceStreamClicked()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        QMessageBox::information(nullptr, QStringLiteral("View FaceStream"), QStringLiteral("Select a clip first."));
        return;
    }

    QString text;
    text += QStringLiteral("Selected clip: %1\n").arg(clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id);
    text += QStringLiteral("Transcript artifact: %1\n\n").arg(m_loadedTranscriptPath);

    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    const bool loadedArtifact = transcriptEngine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot);
    QJsonObject continuityRoot;
    if (loadedArtifact) {
        continuityRoot = continuityRootForClip(artifactRoot, clip->id);
    }
    const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(
        continuityRoot,
        m_loadedTranscriptDoc.object());
    text += QStringLiteral("Imported streams: %1\n").arg(streams.size());
    text += QStringLiteral("Raw tracks: %1\n")
                .arg(continuityRoot.value(QStringLiteral("raw_tracks")).toArray().size());
    const QString importedArtifactDir = continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString();
    if (!importedArtifactDir.isEmpty()) {
        text += QStringLiteral("Imported artifact dir: %1\n").arg(importedArtifactDir);
    }
    QString facestreamPath = continuityRoot.value(QStringLiteral("facestream_part")).toString();
    if (facestreamPath.isEmpty()) {
        facestreamPath = continuityRoot.value(QStringLiteral("facestream_bin")).toString();
    }
    if (facestreamPath.isEmpty()) {
        facestreamPath = continuityRoot.value(QStringLiteral("facestream_ndjson")).toString();
    }
    if (!facestreamPath.isEmpty()) {
        const QFileInfo streamInfo(facestreamPath);
        text += QStringLiteral("facestream checkpoint: %1 (%2 bytes)\n")
                    .arg(facestreamPath)
                    .arg(streamInfo.exists() ? streamInfo.size() : -1);
    }
    const QString processedPath = transcriptEngine.facestreamProcessedArtifactPath(m_loadedTranscriptPath);
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
    const QString identitySidecarPath = transcriptEngine.identityArtifactPath(m_loadedTranscriptPath);
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
    const QString projectRoot = speaker_flow_debug::deriveProjectRootFromTranscriptPath(m_loadedTranscriptPath);
    const QString debugRoot = QDir(projectRoot).absoluteFilePath(QStringLiteral("debug/speaker_flow/%1").arg(clipToken));
    const QString latestRun = speaker_flow_debug::latestRunIdWithArtifact(debugRoot);
    if (!latestRun.isEmpty()) {
        const QString runDir = QDir(debugRoot).absoluteFilePath(latestRun);
        const QString artifactDir = QDir(runDir).absoluteFilePath(QStringLiteral("facestream_artifact"));
        text += QStringLiteral("\nLatest debug run: %1\n").arg(runDir);
        text += QStringLiteral("Latest artifact dir: %1\n").arg(artifactDir);
        const QStringList artifactFiles{
            QStringLiteral("facestream.part"),
            QStringLiteral("tracks.bin"),
            QStringLiteral("continuity_facestream.bin"),
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
            text += QStringLiteral("Debug root exists but contains no facestream_artifact files: %1\n").arg(debugRoot);
        }
    }

    text += QStringLiteral("\n");
    const int row = m_widgets.speakerFaceStreamTable ? m_widgets.speakerFaceStreamTable->currentRow() : -1;
    if (row >= 0 && row < m_faceStreamPanelRows.size()) {
        text += QStringLiteral("Selected stream:\n");
        text += QString::fromUtf8(QJsonDocument(m_faceStreamPanelRows.at(row).toObject()).toJson(QJsonDocument::Indented));
    } else if (!streams.isEmpty()) {
        text += QStringLiteral("All imported streams:\n");
        text += QString::fromUtf8(QJsonDocument(streams).toJson(QJsonDocument::Indented));
    } else {
        text += QStringLiteral("No imported FaceStream paths found for this clip.");
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("View FaceStream"));
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
    const QFileInfo transcriptInfo(m_loadedTranscriptPath);
    const qint64 artifactRevisionMs = facestreamArtifactRevisionMsForTranscript(m_loadedTranscriptPath);
    const QString refreshSignature =
        clip->id + QLatin1Char('|') +
        m_loadedTranscriptPath + QLatin1Char('|') +
        QString::number(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0) +
        QLatin1Char('|') +
        QString::number(artifactRevisionMs);
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
    const QString avatarMediaPath = interactivePreviewMediaPathForClip(*clip);
    std::unique_ptr<editor::DecoderContext> avatarDecoder;
    if (!avatarMediaPath.isEmpty()) {
        avatarDecoder = std::make_unique<editor::DecoderContext>(avatarMediaPath);
        if (!avatarDecoder->initialize()) {
            avatarDecoder.reset();
        }
    }
    QHash<int64_t, QImage> avatarFrameImageCache;
    const qreal sourceFps = resolvedSourceFps(*clip);
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
        const QJsonObject tracking = speakerFramingObject(profileJson);
        const QString trackingMode =
            tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
        const int keyframeCount =
            tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();

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
            &avatarFrameImageCache,
            sourceFps)));
        const uint hueHash = qHash(id);
        const QColor speakerHueTint = QColor::fromHsv(static_cast<int>(hueHash % 360), 160, 92, 105);
        avatarItem->setBackground(QBrush(speakerHueTint));
        avatarItem->setToolTip(
            QStringLiteral("Primary avatar. Unset by default. Click avatar and square-select in Preview to set."));
        avatarItem->setTextAlignment(Qt::AlignCenter);
        avatarItem->setSizeHint(QSize(32, 32));

        auto* displayItem = new QTableWidgetItem(displayLabel);
        displayItem->setData(Qt::UserRole, id);
        displayItem->setData(Qt::UserRole + 10, keyframeCount > 0);
        displayItem->setToolTip(QStringLiteral("Speaker ID: %1\nOrganization: %2\nSummary: %3")
                                    .arg(id,
                                         organization.isEmpty() ? QStringLiteral("None") : organization,
                                         description.isEmpty() ? QStringLiteral("None") : description));
        auto* xItem = new QTableWidgetItem(QString::number(x, 'f', 3));
        xItem->setData(Qt::UserRole, id);
        auto* yItem = new QTableWidgetItem(QString::number(y, 'f', 3));
        yItem->setData(Qt::UserRole, id);
        auto* trackingModeItem = new QTableWidgetItem(
            keyframeCount > 0
                ? QStringLiteral("%1 (%2 keys)").arg(trackingMode).arg(keyframeCount)
                : trackingMode);
        trackingModeItem->setFlags(trackingModeItem->flags() & ~Qt::ItemIsEditable);
        trackingModeItem->setData(Qt::UserRole, id);
        auto* addTrackButton = new QToolButton(m_widgets.speakersTable);
        addTrackButton->setText(QStringLiteral("+"));
        addTrackButton->setToolTip(QStringLiteral("Assign continuity tracks to this speaker."));
        addTrackButton->setAutoRaise(false);
        addTrackButton->setMinimumSize(QSize(28, 28));
        connect(addTrackButton, &QToolButton::clicked, this, [this, id]() {
            selectSpeakerRowById(id);
            m_lastSelectedSpeakerIdHint = id;
            updateSpeakerTrackingStatusLabel();
            updateSelectedSpeakerPanel();
            openTrackPickerForSpeaker(id);
        });
        m_widgets.speakersTable->setItem(row, 0, avatarItem);
        m_widgets.speakersTable->setItem(row, 1, displayItem);
        m_widgets.speakersTable->setItem(row, 2, xItem);
        m_widgets.speakersTable->setItem(row, 3, yItem);
        m_widgets.speakersTable->setItem(row, 4, trackingModeItem);
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
    if (!m_widgets.speakersTable || !m_widgets.speakersTable->selectionModel()) {
        return QString();
    }
    int row = -1;
    const QModelIndexList selectedRows = m_widgets.speakersTable->selectionModel()->selectedRows();
    if (!selectedRows.isEmpty()) {
        row = selectedRows.constFirst().row();
    } else {
        row = m_widgets.speakersTable->currentRow();
    }
    if (row < 0) {
        return QString();
    }
    QTableWidgetItem* speakerItem = m_widgets.speakersTable->item(row, 1);
    if (!speakerItem) {
        return QString();
    }
    return speakerItem->data(Qt::UserRole).toString().trimmed();
}

QString SpeakersTab::speakerDisplayName(const QString& speakerId) const
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return QString();
    }
    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
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
    const bool canEdit = mutableCut && !speakerId.isEmpty();
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool canEditClipFraming = mutableCut && m_speakerDeps.updateClipById && selectedClip;
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

    if (m_widgets.speakerSetReference1Button) {
        m_widgets.speakerSetReference1Button->setEnabled(canEdit);
    }
    if (m_widgets.speakerSetReference2Button) {
        m_widgets.speakerSetReference2Button->setEnabled(canEdit);
    }
    if (m_widgets.speakerClearReferencesButton) {
        m_widgets.speakerClearReferencesButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setEnabled(canEdit);
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("GENERATE FACESTREAM"));
    }
    if (m_widgets.speakerViewFacestreamButton) {
        m_widgets.speakerViewFacestreamButton->setEnabled(true);
    }
    if (m_widgets.speakerFacestreamSettingsButton) {
        m_widgets.speakerFacestreamSettingsButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setEnabled(true);
    }
    if (m_widgets.speakerPrecropFacesButton) {
        m_widgets.speakerPrecropFacesButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerAiFindNamesButton) {
        m_widgets.speakerAiFindNamesButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerAiFindOrganizationsButton) {
        m_widgets.speakerAiFindOrganizationsButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerAiCleanAssignmentsButton) {
        m_widgets.speakerAiCleanAssignmentsButton->setEnabled(canEdit);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        m_widgets.selectedSpeakerPreviousSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        m_widgets.selectedSpeakerNextSentenceButton->setEnabled(!speakerId.isEmpty());
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
    if (m_widgets.speakerPickReference1Button) {
        m_widgets.speakerPickReference1Button->setEnabled(canEdit);
        m_widgets.speakerPickReference1Button->setText(
            m_pendingReferencePick == 1
                ? QStringLiteral("[ARMED] Ref 1")
                : QStringLiteral("Pick Ref 1 (Shift+Drag)"));
    }
    if (m_widgets.speakerPickReference2Button) {
        m_widgets.speakerPickReference2Button->setEnabled(canEdit);
        m_widgets.speakerPickReference2Button->setText(
            m_pendingReferencePick == 2
                ? QStringLiteral("[ARMED] Ref 2")
                : QStringLiteral("Pick Ref 2 (Shift+Drag)"));
    }

    const bool hasClipWideFaceStream =
        selectedClip &&
        (!selectedClip->speakerFramingKeyframes.isEmpty() ||
         !selectedClip->speakerFramingSpeakerId.trimmed().isEmpty());

    if (!mutableCut) {
        m_widgets.speakerTrackingStatusLabel->setText(QString());
        return;
    }
    if (speakerId.isEmpty()) {
        if (!hasClipWideFaceStream && m_loadedTranscriptDoc.isObject()) {
            m_widgets.speakerTrackingStatusLabel->setText(
                QStringLiteral("[MISSING] All-speakers FaceStream artefact has not been created."));
        } else {
            m_widgets.speakerTrackingStatusLabel->setText(QString());
        }
        return;
    }
    if (!m_loadedTranscriptDoc.isObject()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Transcript not loaded."));
        return;
    }

    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    const QJsonArray streams = selectedClip ? continuityStreamsForClip(*selectedClip) : QJsonArray{};
    const int assignedFaceStreamCount =
        selectedClip ? resolvedAssignedTrackIdsForSpeaker(*selectedClip, streams, speakerId).size() : 0;
    if (m_widgets.speakerRefsChipLabel) {
        m_widgets.speakerRefsChipLabel->setText(
            QStringLiteral("Assigned Tracks: %1").arg(assignedFaceStreamCount));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        if (!hasClipWideFaceStream) {
            m_widgets.speakerPointstreamChipLabel->setText(
                QStringLiteral("Continuity Tracks: MISSING (Clip)"));
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
        m_widgets.speakerTrackingChipButton->setEnabled(canEdit && hasClipWideFaceStream);
    }
    if (selectedClip) {
        const bool hasRuntimeBinding = !selectedClip->speakerFramingSpeakerId.trimmed().isEmpty();
        const bool hasFramingData =
            hasRuntimeBinding || !selectedClip->speakerFramingKeyframes.isEmpty();
        const QString clipFramingState = selectedClip->speakerFramingEnabled
            ? QStringLiteral("ON")
            : QStringLiteral("OFF");
        if (m_widgets.speakerClipFramingStatusLabel) {
            m_widgets.speakerClipFramingStatusLabel->setText(
                QStringLiteral("Face Stabilize: %1 | %2")
                    .arg(clipFramingState)
                    .arg(hasRuntimeBinding
                        ? QStringLiteral("Runtime FaceStream")
                        : QStringLiteral("%1 keys").arg(selectedClip->speakerFramingKeyframes.size())));
        }
        if (m_widgets.speakerStabilizeChipButton) {
            const bool canToggleStabilize = canEditFramingTargets && hasFramingData;
            m_widgets.speakerStabilizeChipButton->setText(
                selectedClip->speakerFramingEnabled
                    ? QStringLiteral("Face Stabilize: ON")
                    : QStringLiteral("Face Stabilize: OFF"));
            m_widgets.speakerStabilizeChipButton->setChecked(selectedClip->speakerFramingEnabled);
            m_widgets.speakerStabilizeChipButton->setEnabled(canToggleStabilize);
            if (!selectedClip->speakerFramingEnabled && !hasFramingData) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize needs a FaceStream speaker binding. "
                                   "Run JCut DNN FaceStream Generator with this clip selected."));
            } else if (hasRuntimeBinding) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize uses runtime FaceStream transforms for speaker %1.")
                        .arg(selectedClip->speakerFramingSpeakerId));
            } else {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Click to toggle Face Stabilize for the selected clip."));
            }
        }
        if (m_widgets.speakerApplyFramingToClipCheckBox) {
            const bool canToggleStabilizeFromCheckbox =
                canEditFramingTargets && (selectedClip->speakerFramingEnabled || hasFramingData);
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

    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setEnabled(canEdit);
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("GENERATE FACESTREAM (CONTINUITY)"));
    }

    if (!hasClipWideFaceStream) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("[MISSING] All-speakers FaceStream artefact has not been created. "
                           "Run JCut DNN FaceStream Generator to create it."));
        return;
    }

    m_widgets.speakerTrackingStatusLabel->setText(
        QStringLiteral("Assigned Tracks: %1 | Continuity Tracks: %2 | Speaker Tracking: %3 | Face Stabilize: %4")
            .arg(assignedFaceStreamCount)
            .arg(QStringLiteral("ClipWide Ready"))
            .arg(trackingEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg((selectedClip && selectedClip->speakerFramingEnabled) ? QStringLiteral("ON") : QStringLiteral("OFF")));
}

