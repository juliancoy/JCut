#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_flow_debug.h"

#include "boxstream_generation.h"
#include "boxstream_runtime.h"
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

using namespace jcut::boxstream;

void SpeakersTab::onSpeakerBoxStreamSettingsClicked()
{
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("FaceStream Settings"));
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
            "1. Run Pre-crop Faces (FaceFind) first.\n"
            "2. Identify all unique faces and resolve duplicates/unknowns.\n"
            "3. Click Generate FaceStream.\n"
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
