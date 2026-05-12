#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_flow_debug.h"

#include "facestream_generation.h"
#include "facestream_runtime.h"
#include "decoder_context.h"
#include "render_internal.h"
#include "transcript_engine.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
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
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

#if JCUT_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#if JCUT_HAVE_OPENCV_CONTRIB
#include <opencv2/tracking.hpp>
#include <opencv2/tracking/tracking_legacy.hpp>
#endif

#include <vulkan/vulkan.h>
#endif

using namespace jcut::facestream;

void SpeakersTab::onSpeakerFaceStreamSettingsClicked()
{
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("JCut Detector Settings"));
    dialog.setWindowFlag(Qt::Window, true);
    dialog.resize(520, 230);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* infoLabel = new QLabel(
        QStringLiteral("Configure FaceStream-related smoothing options.\n\n"
                       "These are not part of FaceStream preflight so run setup stays minimal.\n"
                       "Generate FaceStream itself does not apply clip transforms."),
        &dialog);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto* smoothTranslationCheck =
        new QCheckBox(QStringLiteral("Smooth translation keyframes (post-solve)"), &dialog);
    smoothTranslationCheck->setChecked(g_facestreamSmoothingSettings.smoothTranslation);
    smoothTranslationCheck->setToolTip(
        QStringLiteral("Applies gentle post-solve smoothing to translation keys to reduce jitter and micro-shakes."));
    layout->addWidget(smoothTranslationCheck);

    auto* smoothScaleCheck =
        new QCheckBox(QStringLiteral("Smooth scale keyframes (post-solve)"), &dialog);
    smoothScaleCheck->setChecked(g_facestreamSmoothingSettings.smoothScale);
    smoothScaleCheck->setToolTip(
        QStringLiteral("Applies gentle post-solve smoothing to scale keys to reduce zoom pumping and abrupt size changes."));
    layout->addWidget(smoothScaleCheck);

    auto* noteLabel = new QLabel(
        QStringLiteral("Current default is OFF for all smoothing."),
        &dialog);
    noteLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    layout->addWidget(noteLabel);

    auto* buttons = new QHBoxLayout;
    auto* rebuildProcessedButton = new QPushButton(QStringLiteral("Rebuild Processed Sidecar"), &dialog);
    rebuildProcessedButton->setToolTip(
        QStringLiteral("Recreate the FaceStreamProcessed sidecar from the raw FaceStream detections for the selected clip."));
    buttons->addWidget(rebuildProcessedButton);
    buttons->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* saveButton = new QPushButton(QStringLiteral("Save"), &dialog);
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    layout->addLayout(buttons);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(rebuildProcessedButton, &QPushButton::clicked, &dialog, [this]() {
        rebuildProcessedFaceStreamForSelectedClip(true);
    });

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    g_facestreamSmoothingSettings.smoothTranslation = smoothTranslationCheck->isChecked();
    g_facestreamSmoothingSettings.smoothScale = smoothScaleCheck->isChecked();
}

void SpeakersTab::onSpeakerGuideClicked()
{
    const QString guideText =
        QStringLiteral(
            "Subtitle Face Tracking Guide\n\n"
            "1. Generate FaceStream first.\n"
            "2. Click Assign FaceStreams to extract one representative crop per FaceStream track.\n"
            "3. Review or auto-apply track-to-speaker assignments.\n"
            "4. The system detects and tracks face continuity across transcript time.\n"
            "5. Independent FaceStreams are created per continuity track (T<id>).\n"
            "6. Optional: enable dialogue-only scanning in preflight.\n\n"
            "FaceBox Target\n"
            "- In Speakers, set Face X / Face Y for desired on-screen face position.\n"
            "- Toggle Show FaceBox to show/hide the yellow target box in Preview.\n"
            "- The yellow box in Preview is the target box faces are fit into.\n\n"
            "Face Stabilize\n"
            "- Face Stabilize is a separate clip-level toggle.\n"
            "- It applies generated face keyframes to the selected clip.\n\n"
            "Range Policy\n"
            "- Generate FaceStream scans transcript-global continuity by default.\n"
            "- It is not limited to the selected clip's source-in/source-out range.\n\n"
            "Tips\n"
            "- Square selection is required and enforced.\n"
            "- Identity mapping can be done later in FaceFind; generation is identity-agnostic.\n"
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

bool SpeakersTab::handlePreviewFaceStreamBox(const QString& clipId,
                                             int trackId,
                                             const QString& streamId,
                                             int64_t sourceFrame,
                                             qreal xNorm,
                                             qreal yNorm,
                                             qreal boxSizeNorm)
{
    if (!activeCutMutable() || trackId < 0) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->id != clipId) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        updateSpeakerTrackingStatusLabel();
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
    QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    QJsonObject clipRoot = clipsRoot.value(clipId).toObject();
    clipRoot[QStringLiteral("clip_id")] = clipId;
    clipRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonObject resolvedPayload = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    resolvedPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonArray resolvedMap = resolvedPayload.value(QStringLiteral("track_identity_map")).toArray();
    QJsonArray nextResolvedMap;
    bool replaced = false;
    for (const QJsonValue& value : resolvedMap) {
        QJsonObject row = value.toObject();
        const QString rowIdentityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
        const int rowTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
        const QString rowStreamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
        const int64_t rowAnchorSourceFrame =
            row.contains(QString(kSpeakerFlowAnchorSourceFrameKey))
                ? row.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong()
                : -1;
        const qreal rowAnchorX =
            row.value(QString(kSpeakerFlowAnchorXKey)).toDouble(-1.0);
        const qreal rowAnchorY =
            row.value(QString(kSpeakerFlowAnchorYKey)).toDouble(-1.0);
        if ((rowIdentityId == speakerId &&
             rowAnchorSourceFrame == sourceFrame &&
             std::abs(rowAnchorX - xNorm) < 0.001 &&
             std::abs(rowAnchorY - yNorm) < 0.001)) {
            row[QStringLiteral("identity_id")] = speakerId;
            row[QStringLiteral("stream_id")] = streamId;
            row[QString(kSpeakerFlowAnchorSourceFrameKey)] = static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
            row[QString(kSpeakerFlowAnchorXKey)] = qBound<qreal>(0.0, xNorm, 1.0);
            row[QString(kSpeakerFlowAnchorYKey)] = qBound<qreal>(0.0, yNorm, 1.0);
            row[QString(kSpeakerFlowAnchorBoxSizeKey)] = qBound<qreal>(0.01, boxSizeNorm, 1.0);
            row[QStringLiteral("resolution_source")] = QStringLiteral("preview_click");
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
        row[QStringLiteral("resolution_source")] = QStringLiteral("preview_click");
        row[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        nextResolvedMap.push_back(row);
    }
    resolvedPayload[QStringLiteral("track_identity_map")] = nextResolvedMap;
    clipRoot[QStringLiteral("resolved_current")] = resolvedPayload;

    QJsonObject humanRuns = clipRoot.value(QStringLiteral("human_runs")).toObject();
    const QString runId = QStringLiteral("preview_click_%1").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")));
    QJsonObject humanPayload;
    humanPayload[QStringLiteral("run_id")] = runId;
    humanPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonArray auditLog;
    QJsonObject auditRow;
    auditRow[QStringLiteral("timestamp_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    auditRow[QStringLiteral("action")] = QStringLiteral("preview_face_stream_identity_set");
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
        if (faceRef.value(QStringLiteral("track_id")).toInt(-1) == trackId &&
            faceRef.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong() == sourceFrame &&
            std::abs(faceRef.value(QString(kSpeakerFlowAnchorXKey)).toDouble(-1.0) - xNorm) < 0.001 &&
            std::abs(faceRef.value(QString(kSpeakerFlowAnchorYKey)).toDouble(-1.0) - yNorm) < 0.001) {
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
        faceRef[QStringLiteral("source")] = QStringLiteral("preview_click");
        faceRefs.push_back(faceRef);
        profile[QString(kTranscriptSpeakerFaceRefsKey)] = faceRefs;
        profiles[speakerId] = profile;
        transcriptRoot[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    }

    clipsRoot[clipId] = clipRoot;
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
    QJsonObject assignmentRoot = assignmentsByClip.value(clipId).toObject();
    assignmentRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    assignmentRoot[QStringLiteral("track_identity_map")] = nextResolvedMap;
    assignmentsByClip[clipId] = assignmentRoot;
    identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
    identityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    identityRoot[QStringLiteral("identity_assignments_by_clip")] = assignmentsByClip;
    engine.saveIdentityArtifact(m_loadedTranscriptPath, identityRoot);

    m_boxStreamPanelRefreshSignature.clear();
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
