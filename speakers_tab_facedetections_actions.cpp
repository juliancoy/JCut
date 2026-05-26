#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_flow_debug.h"

#include "facedetections_generation.h"
#include "facedetections_runtime.h"
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

using namespace jcut::facedetections;

void SpeakersTab::onSpeakerFaceDetectionsSettingsClicked()
{
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Continuity Track Tools"));
    dialog.setWindowFlag(Qt::Window, true);
    dialog.resize(520, 260);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* infoLabel = new QLabel(
        QStringLiteral("Configure continuity-track smoothing options.\n\n"
                       "These are not part of detector preflight so run setup stays minimal.\n"
                       "Generating continuity tracks itself does not apply clip transforms."),
        &dialog);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto* smoothTranslationCheck =
        new QCheckBox(QStringLiteral("Smooth translation keyframes (post-solve)"), &dialog);
    smoothTranslationCheck->setChecked(g_facedetectionsSmoothingSettings.smoothTranslation);
    smoothTranslationCheck->setToolTip(
        QStringLiteral("Applies gentle post-solve smoothing to translation keys to reduce jitter and micro-shakes."));
    layout->addWidget(smoothTranslationCheck);

    auto* smoothScaleCheck =
        new QCheckBox(QStringLiteral("Smooth scale keyframes (post-solve)"), &dialog);
    smoothScaleCheck->setChecked(g_facedetectionsSmoothingSettings.smoothScale);
    smoothScaleCheck->setToolTip(
        QStringLiteral("Applies gentle post-solve smoothing to scale keys to reduce zoom pumping and abrupt size changes."));
    layout->addWidget(smoothScaleCheck);

    auto* showPreviewCheck = new QCheckBox(QStringLiteral("Show Preview"), &dialog);
    showPreviewCheck->setChecked(
        m_widgets.speakerShowFaceDetectionsBoxesCheckBox &&
        m_widgets.speakerShowFaceDetectionsBoxesCheckBox->isChecked());
    showPreviewCheck->setToolTip(
        QStringLiteral("Draw generated continuity-track boxes for the selected clip in Preview."));
    layout->addWidget(showPreviewCheck);

    auto* noteLabel = new QLabel(
        QStringLiteral("Current default is OFF for all smoothing."),
        &dialog);
    noteLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    layout->addWidget(noteLabel);

    auto* buttons = new QHBoxLayout;
    auto* rebuildProcessedButton = new QPushButton(QStringLiteral("Rebuild Continuity Tracks"), &dialog);
    rebuildProcessedButton->setToolTip(
        QStringLiteral("Recreate processed continuity tracks from the raw detections for the selected clip."));
    buttons->addWidget(rebuildProcessedButton);
    buttons->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* saveButton = new QPushButton(QStringLiteral("Save"), &dialog);
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    layout->addLayout(buttons);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(showPreviewCheck, &QCheckBox::toggled, &dialog, [this](bool checked) {
        if (m_widgets.speakerShowFaceDetectionsBoxesCheckBox) {
            m_widgets.speakerShowFaceDetectionsBoxesCheckBox->setChecked(checked);
        } else if (m_speakerDeps.refreshPreview) {
            m_speakerDeps.refreshPreview();
        }
    });
    connect(rebuildProcessedButton, &QPushButton::clicked, &dialog, [this]() {
        rebuildProcessedFaceDetectionsForSelectedClip(true);
    });

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    g_facedetectionsSmoothingSettings.smoothTranslation = smoothTranslationCheck->isChecked();
    g_facedetectionsSmoothingSettings.smoothScale = smoothScaleCheck->isChecked();
}

void SpeakersTab::onSpeakerGuideClicked()
{
    const QString guideText =
        QStringLiteral(
            "Speaker Flow Guide\n\n"
            "1. Generate Continuity Tracks first.\n"
            "2. Click Assign Speaker Identity to extract representative identity crops per continuity track.\n"
            "3. Review or auto-apply track-to-speaker assignments.\n"
            "4. The system detects and tracks face continuity across transcript time.\n"
            "5. Independent continuity tracks are created with stable track IDs (T<id>).\n"
            "6. Optional: enable dialogue-only scanning in preflight.\n\n"
            "Target Box\n"
            "- In Speakers, set Target X / Target Y for the desired on-screen face position.\n"
            "- Enable Target Box to show/hide the yellow target box in Preview.\n"
            "- The yellow box in Preview is the target box faces are fit into.\n\n"
            "Face Stabilize\n"
            "- Face Stabilize is a separate clip-level toggle.\n"
            "- It applies generated face keyframes to the selected clip.\n\n"
            "Range Policy\n"
            "- Generate Continuity Tracks scans transcript-global continuity by default.\n"
            "- It is not limited to the selected clip's source-in/source-out range.\n\n"
            "Tips\n"
            "- Square selection is required and enforced.\n"
            "- Speaker identity assignment happens later in Assign Speaker Identity; generation is identity-agnostic.\n"
            "- Speaker metadata is editable only on derived cuts (not Original).");
    QMessageBox::information(nullptr, QStringLiteral("Speaker Flow Guide"), guideText);
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
    Q_UNUSED(clipId);
    Q_UNUSED(xNorm);
    Q_UNUSED(yNorm);
    return false;
}

bool SpeakersTab::handlePreviewBox(const QString& clipId,
                                   qreal xNorm,
                                   qreal yNorm,
                                   qreal boxSizeNorm)
{
    Q_UNUSED(clipId);
    Q_UNUSED(xNorm);
    Q_UNUSED(yNorm);
    Q_UNUSED(boxSizeNorm);
    return false;
}
