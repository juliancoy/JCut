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
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QImage>
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
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>


SpeakersTab::SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : TableTabBase(deps, parent)
    , m_widgets(widgets)
    , m_speakerDeps(deps)
{
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
    return true;
}

bool SpeakersTab::saveLoadedTranscriptDocument()
{
    if (m_loadedTranscriptPath.trimmed().isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

void SpeakersTab::wire()
{
    if (!m_boxStreamPanelRefreshTimer) {
        m_boxStreamPanelRefreshTimer = new QTimer(this);
        m_boxStreamPanelRefreshTimer->setSingleShot(true);
        m_boxStreamPanelRefreshTimer->setInterval(40);
        connect(m_boxStreamPanelRefreshTimer, &QTimer::timeout, this, [this]() {
            m_boxStreamPanelRefreshQueued = false;
            refreshFaceStreamPathsPanel();
        });
    }
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_widgets.speakersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_widgets.speakersTable->setItemDelegateForColumn(
            2, new SpeakerNameItemDelegate(m_widgets.speakersTable));
        connect(m_widgets.speakersTable, &QTableWidget::itemChanged,
                this, &SpeakersTab::onSpeakersTableItemChanged);
        connect(m_widgets.speakersTable, &QTableWidget::itemClicked,
                this, &SpeakersTab::onSpeakersTableItemClicked);
        connect(m_widgets.speakersTable, &QTableWidget::itemSelectionChanged,
                this, &SpeakersTab::onSpeakersSelectionChanged);
        connect(m_widgets.speakersTable, &QWidget::customContextMenuRequested,
                this, &SpeakersTab::onSpeakersTableContextMenuRequested);
        if (SpeakersTable* speakersTable =
                qobject_cast<SpeakersTable*>(m_widgets.speakersTable)) {
            connect(speakersTable, &SpeakersTable::avatarHoverRequested, this,
                    [this](const QString& speakerId, const QPoint& globalPos) {
                        showSpeakerAvatarHoverPreview(speakerId, globalPos);
                    });
            connect(speakersTable, &SpeakersTable::avatarHoverCleared, this,
                    [this]() { hideSpeakerAvatarHoverPreview(); });
        }
    }
    if (m_widgets.speakerFaceStreamTable) {
        connect(m_widgets.speakerFaceStreamTable, &QTableWidget::itemSelectionChanged, this, [this]() {
            if (!m_widgets.speakerFaceStreamTable || !m_widgets.speakerFaceStreamDetailsEdit) {
                return;
            }
            const int row = m_widgets.speakerFaceStreamTable->currentRow();
            if (row < 0) {
                m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
                    QStringLiteral("Select a FaceStream path row to inspect full JSON."));
                return;
            }
            QTableWidgetItem* streamItem = m_widgets.speakerFaceStreamTable->item(row, 0);
            const int64_t frame30 = streamItem
                ? streamItem->data(Qt::UserRole + 2).toLongLong()
                : -1;
            if (frame30 >= 0 && m_deps.seekToTimelineFrame) {
                const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
                if (clip) {
                    const qreal sourceFps = resolvedSourceFps(*clip);
                    const int64_t sourceFrame = qMax<int64_t>(
                        0, static_cast<int64_t>(std::floor((static_cast<qreal>(frame30) / kTimelineFps) * sourceFps)));
                    int64_t timelineFrame = clip->startFrame + (sourceFrame - clip->sourceInFrame);
                    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
                    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
                    m_deps.seekToTimelineFrame(timelineFrame);
                }
            }
            QString streamJson;
            if (streamItem) {
                const int rowIndex = streamItem->data(Qt::UserRole + 1).toInt();
                if (rowIndex >= 0 && rowIndex < m_boxStreamPanelRows.size()) {
                    const QJsonObject streamObj = m_boxStreamPanelRows.at(rowIndex).toObject();
                    streamJson = QString::fromUtf8(QJsonDocument(streamObj).toJson(QJsonDocument::Indented));
                }
            }
            m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
                streamJson.isEmpty()
                    ? QStringLiteral("No stream payload available.")
                    : streamJson);
        });
    }
    if (m_widgets.speakerSetReference1Button) {
        connect(m_widgets.speakerSetReference1Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerSetReference1Clicked);
    }
    if (m_widgets.speakerSetReference2Button) {
        connect(m_widgets.speakerSetReference2Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerSetReference2Clicked);
    }
    if (m_widgets.speakerPickReference1Button) {
        m_widgets.speakerPickReference1Button->setToolTip(
            QStringLiteral("Required baseline. Arm Ref 1 pick mode, then in Preview hold Shift and drag a square over the speaker head for framing."));
        connect(m_widgets.speakerPickReference1Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPickReference1Clicked);
    }
    if (m_widgets.speakerPickReference2Button) {
        m_widgets.speakerPickReference2Button->setToolTip(
            QStringLiteral("Optional quality boost. Arm Ref 2 and pick another frame for better framing interpolation."));
        connect(m_widgets.speakerPickReference2Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPickReference2Clicked);
    }
    if (m_widgets.speakerClearReferencesButton) {
        connect(m_widgets.speakerClearReferencesButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerClearReferencesClicked);
    }
    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("JCUT DNN FACESTREAM"));
        m_widgets.speakerRunAutoTrackButton->setMinimumHeight(40);
        m_widgets.speakerRunAutoTrackButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background:#f4b53f;"
            "  color:#121820;"
            "  border:1px solid #9b6e10;"
            "  border-radius:6px;"
            "  font-weight:700;"
            "  padding:6px 10px;"
            "}"
            "QPushButton:disabled {"
            "  background:#5c4c2b;"
            "  color:#b8a783;"
            "  border:1px solid #6a5731;"
            "}"));
        m_widgets.speakerRunAutoTrackButton->setToolTip(
            QStringLiteral("Run the default JCut DNN FaceStream Generator for all face tracks."));
        connect(m_widgets.speakerRunAutoTrackButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRunAutoTrackClicked);
    }
    if (m_widgets.speakerViewFacestreamButton) {
        m_widgets.speakerViewFacestreamButton->setToolTip(
            QStringLiteral("Open the selected FaceStream JSON and the latest generated artifact paths."));
        connect(m_widgets.speakerViewFacestreamButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerViewFaceStreamClicked);
    }
    if (m_widgets.speakerFacestreamSettingsButton) {
        m_widgets.speakerFacestreamSettingsButton->setToolTip(
            QStringLiteral("Open FaceStream-specific runtime smoothing options."));
        connect(m_widgets.speakerFacestreamSettingsButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerFaceStreamSettingsClicked);
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setToolTip(
            QStringLiteral("Click to toggle Subtitle Face Tracking for the selected speaker."));
        connect(m_widgets.speakerTrackingChipButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerTrackingChipClicked);
    }
    if (m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setToolTip(
            QStringLiteral("Click to toggle Face Stabilize for the selected clip."));
        connect(m_widgets.speakerStabilizeChipButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerStabilizeChipClicked);
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setToolTip(
            QStringLiteral("Open a quick guide for continuity FaceStream generation and mapping."));
        connect(m_widgets.speakerGuideButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerGuideClicked);
    }
    if (m_widgets.speakerPrecropFacesButton) {
        m_widgets.speakerPrecropFacesButton->setToolTip(
            QStringLiteral("Extract one comparison crop per generated FaceStream track and assign tracks to transcript speakers."));
        connect(m_widgets.speakerPrecropFacesButton, &QPushButton::clicked, this, [this]() {
            // Keep REST/UI click handlers from blocking while the assignment preflight starts.
            QTimer::singleShot(0, this, &SpeakersTab::onSpeakerPrecropFacesClicked);
        });
    }
    if (m_widgets.speakerAiFindNamesButton) {
        m_widgets.speakerAiFindNamesButton->setToolTip(
            QStringLiteral("Mine transcript text with AI and overwrite existing speaker names when stronger candidates are found."));
        connect(m_widgets.speakerAiFindNamesButton, &QPushButton::clicked, this, [this]() {
            runAiFindSpeakerNames();
        });
    }
    if (m_widgets.speakerAiFindOrganizationsButton) {
        m_widgets.speakerAiFindOrganizationsButton->setToolTip(
            QStringLiteral("Infer likely organizations/entities mentioned by each speaker."));
        connect(m_widgets.speakerAiFindOrganizationsButton, &QPushButton::clicked, this, [this]() {
            runAiFindOrganizations();
        });
    }
    if (m_widgets.speakerAiCleanAssignmentsButton) {
        m_widgets.speakerAiCleanAssignmentsButton->setToolTip(
            QStringLiteral("Remove one-off/spurious speaker word assignments with safe reassignment."));
        connect(m_widgets.speakerAiCleanAssignmentsButton, &QPushButton::clicked, this, [this]() {
            runAiCleanSpuriousAssignments();
        });
    }
    if (m_widgets.speakerFramingTargetXSpin) {
        connect(m_widgets.speakerFramingTargetXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        connect(m_widgets.speakerFramingTargetYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingTargetBoxSpin) {
        connect(m_widgets.speakerFramingTargetBoxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        connect(m_widgets.speakerFramingZoomEnabledCheckBox, &QCheckBox::toggled,
                this, &SpeakersTab::onSpeakerFramingZoomEnabledChanged);
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        connect(m_widgets.speakerApplyFramingToClipCheckBox, &QCheckBox::toggled,
                this, &SpeakersTab::onSpeakerApplyFramingToClipChanged);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        connect(m_widgets.selectedSpeakerPreviousSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPreviousSentenceClicked);
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        connect(m_widgets.selectedSpeakerNextSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerNextSentenceClicked);
    }
    if (m_widgets.selectedSpeakerRandomSentenceButton) {
        connect(m_widgets.selectedSpeakerRandomSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRandomSentenceClicked);
    }
    if (m_widgets.selectedSpeakerRef1ImageLabel) {
        m_widgets.selectedSpeakerRef1ImageLabel->installEventFilter(this);
        m_widgets.selectedSpeakerRef1ImageLabel->setCursor(Qt::PointingHandCursor);
        m_widgets.selectedSpeakerRef1ImageLabel->setToolTip(
            QStringLiteral("Click: open Ref 1 preview/FaceFind. Shift+Drag: adjust crop."));
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->installEventFilter(this);
        m_widgets.selectedSpeakerRef2ImageLabel->setCursor(Qt::PointingHandCursor);
        m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(
            QStringLiteral("Click: open Ref 2 preview/FaceFind. Shift+Drag: adjust crop."));
    }
}

bool SpeakersTab::eventFilter(QObject* watched, QEvent* event)
{
    QLabel* label = qobject_cast<QLabel*>(watched);
    const bool isRef1 = watched == m_widgets.selectedSpeakerRef1ImageLabel;
    const bool isRef2 = watched == m_widgets.selectedSpeakerRef2ImageLabel;
    if (!isRef1 && !isRef2) {
        return TableTabBase::eventFilter(watched, event);
    }
    const int referenceIndex = isRef1 ? 1 : 2;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() != Qt::LeftButton) {
            break;
        }
        const bool dragModifierPressed = (mouseEvent->modifiers() & Qt::ShiftModifier);
        if (dragModifierPressed && beginSelectedReferenceAvatarDrag(referenceIndex, mouseEvent->pos())) {
            if (label) {
                label->setCursor(Qt::ClosedHandCursor);
                label->grabMouse();
            }
            mouseEvent->accept();
            return true;
        }
        openReferencePreviewWindow(referenceIndex);
        mouseEvent->accept();
        return true;
        break;
    }
    case QEvent::MouseMove: {
        if (!m_selectedAvatarDragActive || m_selectedAvatarDragReferenceIndex != referenceIndex) {
            break;
        }
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        updateSelectedReferenceAvatarDrag(mouseEvent->pos());
        mouseEvent->accept();
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (!m_selectedAvatarDragActive || m_selectedAvatarDragReferenceIndex != referenceIndex ||
            mouseEvent->button() != Qt::LeftButton) {
            break;
        }
        finishSelectedReferenceAvatarDrag(true);
        if (label) {
            label->releaseMouse();
            label->setCursor(Qt::PointingHandCursor);
        }
        mouseEvent->accept();
        return true;
    }
    case QEvent::Wheel: {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        if (adjustSelectedReferenceAvatarZoom(referenceIndex, wheelEvent->angleDelta().y())) {
            wheelEvent->accept();
            return true;
        }
        break;
    }
    case QEvent::Hide:
    case QEvent::Destroy: {
        if (m_selectedAvatarDragActive && m_selectedAvatarDragReferenceIndex == referenceIndex) {
            finishSelectedReferenceAvatarDrag(false);
            if (label) {
                label->releaseMouse();
                label->setCursor(Qt::PointingHandCursor);
            }
        }
        break;
    }
    default:
        break;
    }
    return TableTabBase::eventFilter(watched, event);
}

bool SpeakersTab::clipSupportsTranscript(const TimelineClip& clip) const
{
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}

QPixmap SpeakersTab::faceStreamPreviewAvatar(const TimelineClip& clip,
                                             const QString& speakerId,
                                             const QJsonObject& keyframeObj,
                                             int size) const
{
    if (keyframeObj.isEmpty()) {
        return placeholderSpeakerAvatar(speakerId);
    }

    const int avatarSize = qMax(24, size);
    const int64_t sourceFrame30 = qMax<int64_t>(
        0, keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong());
    const qreal locX = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
    const qreal locY = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0,
        keyframeObj.value(QStringLiteral("box_size")).toDouble(
            keyframeObj.value(QStringLiteral("box")).toDouble(-1.0)),
        1.0);
    const qreal boxLeft = keyframeObj.value(QStringLiteral("box_left")).toDouble(-1.0);
    const qreal boxTop = keyframeObj.value(QStringLiteral("box_top")).toDouble(-1.0);
    const qreal boxRight = keyframeObj.value(QStringLiteral("box_right")).toDouble(-1.0);
    const qreal boxBottom = keyframeObj.value(QStringLiteral("box_bottom")).toDouble(-1.0);
    const QString cacheKey = QStringLiteral("facestream|%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(m_loadedTranscriptPath)
        .arg(clip.id)
        .arg(speakerId)
        .arg(sourceFrame30)
        .arg(static_cast<int>(std::round(locX * 1000.0)))
        .arg(static_cast<int>(std::round(locY * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxSizeNorm) * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxLeft) * 1000.0)))
        .arg(avatarSize);
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);
    editor::DecoderContext ctx(mediaPath);
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    if (ctx.initialize()) {
        const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
        const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        if (!image.isNull() && image.width() > 0 && image.height() > 0) {
            const int width = image.width();
            const int height = image.height();
            const int minSide = qMax(1, qMin(width, height));
            QRect cropRect;
            if (boxLeft >= 0.0 && boxTop >= 0.0 && boxRight > boxLeft && boxBottom > boxTop &&
                boxRight <= 1.0 && boxBottom <= 1.0) {
                const int left = qBound(0, static_cast<int>(std::floor(boxLeft * width)), qMax(0, width - 1));
                const int top = qBound(0, static_cast<int>(std::floor(boxTop * height)), qMax(0, height - 1));
                const int right = qBound(left + 1, static_cast<int>(std::ceil(boxRight * width)), width);
                const int bottom = qBound(top + 1, static_cast<int>(std::ceil(boxBottom * height)), height);
                cropRect = QRect(left, top, right - left, bottom - top);
            }
            if (!cropRect.isValid() || cropRect.isEmpty()) {
                int side = qMax(40, minSide / 3);
                if (boxSizeNorm > 0.0) {
                    side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
                }
                const int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
                const int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
                const int left = qBound(0, cx - (side / 2), qMax(0, width - side));
                const int top = qBound(0, cy - (side / 2), qMax(0, height - side));
                cropRect = QRect(left, top, qMin(side, width - left), qMin(side, height - top));
            }
            QImage crop = image.copy(cropRect)
                              .scaled(avatarSize, avatarSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (!crop.isNull()) {
                QImage rounded(avatarSize, avatarSize, QImage::Format_ARGB32_Premultiplied);
                rounded.fill(Qt::transparent);
                QPainter painter(&rounded);
                painter.setRenderHint(QPainter::Antialiasing, true);
                QPainterPath path;
                path.addRoundedRect(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0), 8.0, 8.0);
                painter.setClipPath(path);
                painter.drawImage(QRect(0, 0, avatarSize, avatarSize), crop);
                painter.setClipping(false);
                painter.setPen(QPen(QColor(QStringLiteral("#f4d35e")), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0), 8.0, 8.0);
                avatar = QPixmap::fromImage(rounded);
            }
        }
    }

    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QVector<QPixmap> SpeakersTab::assignedFaceStreamPreviewPixmaps(const TimelineClip& clip,
                                                               const QString& speakerId) const
{
    QVector<QPixmap> pixmaps;
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return pixmaps;
    }

    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonArray faceRefs = speakerFaceRefs(profiles.value(speakerId).toObject());
    for (const QJsonValue& faceRefValue : faceRefs) {
        const QJsonObject previewKeyframe = previewKeyframeFromSpeakerFaceRef(faceRefValue.toObject());
        if (!previewKeyframe.isEmpty()) {
            pixmaps.push_back(faceStreamPreviewAvatar(clip, speakerId, previewKeyframe));
        }
    }
    if (!pixmaps.isEmpty()) {
        return pixmaps;
    }

    const QJsonArray streams = continuityStreamsForClip(clip);
    const QVector<int> assignedTrackIdList =
        resolvedAssignedTrackIdsForSpeaker(clip, streams, speakerId);
    QSet<int> assignedTrackIds;
    for (int trackId : assignedTrackIdList) {
        assignedTrackIds.insert(trackId);
    }
    if (assignedTrackIds.isEmpty()) {
        return pixmaps;
    }
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (!assignedTrackIds.contains(trackId)) {
            continue;
        }
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (keyframes.isEmpty()) {
            continue;
        }
        const QJsonObject keyframeObj = keyframes.first().toObject();
        if (keyframeObj.isEmpty()) {
            continue;
        }
        pixmaps.push_back(faceStreamPreviewAvatar(clip, speakerId, keyframeObj));
    }
    return pixmaps;
}

QJsonArray SpeakersTab::continuityStreamsForClip(const TimelineClip& clip) const
{
    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    if (transcriptEngine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot)) {
        const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
        const QJsonObject continuityRoot = byClip.value(clip.id.trimmed()).toObject();
        const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(
            continuityRoot,
            m_loadedTranscriptDoc.object());
        if (!streams.isEmpty()) {
            return streams;
        }
    }
    const QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject speakerFlow = root.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject continuityRoot = clipFlow.value(QStringLiteral("continuity_facestreams")).toObject();
    return continuityRoot.value(QStringLiteral("streams")).toArray();
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
    QJsonObject byClip = rawArtifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
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
    rawArtifactRoot[QStringLiteral("continuity_facestreams_by_clip")] = byClip;
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

    if (interactive) {
        const QFileInfo processedInfo(engine.facestreamProcessedArtifactPath(m_loadedTranscriptPath));
        QMessageBox::information(
            nullptr,
            QStringLiteral("Rebuild Processed FaceStream"),
            QStringLiteral("Rebuilt processed FaceStream sidecar.\n\n%1")
                .arg(processedInfo.absoluteFilePath()));
    }
    requestRefreshFaceStreamPathsPanel();
    if (m_speakerDeps.refreshPreview) {
        m_speakerDeps.refreshPreview();
    }
    return true;
}

QJsonObject SpeakersTab::resolveFaceStreamAssignmentRow(const TimelineClip& clip,
                                                        const QJsonArray& streams,
                                                        const QJsonObject& row) const
{
    const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
    if (identityId.isEmpty() || streams.isEmpty()) {
        return {};
    }

    const int storedTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
    const QString storedStreamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
    const bool hasAnchor =
        row.contains(QString(kSpeakerFlowAnchorSourceFrameKey)) &&
        row.contains(QString(kSpeakerFlowAnchorXKey)) &&
        row.contains(QString(kSpeakerFlowAnchorYKey)) &&
        row.contains(QString(kSpeakerFlowAnchorBoxSizeKey));
    const int64_t anchorSourceFrame =
        row.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong();
    const qreal anchorX = qBound<qreal>(0.0, row.value(QString(kSpeakerFlowAnchorXKey)).toDouble(0.5), 1.0);
    const qreal anchorY = qBound<qreal>(0.0, row.value(QString(kSpeakerFlowAnchorYKey)).toDouble(0.5), 1.0);
    const qreal anchorBox = qBound<qreal>(0.01, row.value(QString(kSpeakerFlowAnchorBoxSizeKey)).toDouble(0.2), 1.0);
    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};

    QJsonObject bestResolved;
    double bestScore = std::numeric_limits<double>::max();
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (trackId < 0 || keyframes.isEmpty()) {
            continue;
        }

        if (!hasAnchor) {
            if ((storedTrackId >= 0 && trackId == storedTrackId) ||
                (!storedStreamId.isEmpty() && streamId == storedStreamId)) {
                QJsonObject resolved = row;
                resolved[QStringLiteral("track_id")] = trackId;
                resolved[QStringLiteral("stream_id")] = streamId;
                return resolved;
            }
            continue;
        }

        int64_t streamFrameMin = std::numeric_limits<int64_t>::max();
        int64_t streamFrameMax = -1;
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            const int64_t frame =
                keyframeObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            streamFrameMin = qMin<int64_t>(streamFrameMin, frame);
            streamFrameMax = qMax<int64_t>(streamFrameMax, frame);
        }
        const FacestreamFrameDomain frameDomain =
            inferFacestreamFrameDomain(clip, streamFrameMin, streamFrameMax);

        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            const int64_t frame =
                keyframeObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const int64_t sourceFrame =
                mapFacestreamFrameToSourceFrame(clip, frame, frameDomain, renderSyncMarkers);
            const qreal x =
                qBound<qreal>(0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const qreal y =
                qBound<qreal>(0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
            const qreal box =
                qBound<qreal>(0.01, keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2), 1.0);
            const qreal posDist = std::hypot(x - anchorX, y - anchorY);
            const qreal boxDist = std::abs(box - anchorBox);
            const qreal frameDist =
                static_cast<qreal>(std::llabs(sourceFrame - anchorSourceFrame));
            const double score = (frameDist / 12.0) + (posDist * 6.0) + (boxDist * 3.0);
            if (score < bestScore) {
                bestScore = score;
                bestResolved = row;
                bestResolved[QStringLiteral("track_id")] = trackId;
                bestResolved[QStringLiteral("stream_id")] = streamId;
            }
        }
    }

    if (!bestResolved.isEmpty()) {
        return bestResolved;
    }

    if (storedTrackId >= 0 || !storedStreamId.isEmpty()) {
        QJsonObject fallback = row;
        return fallback;
    }
    return {};
}

QHash<int, QString> SpeakersTab::resolvedIdentityByTrackId(const TimelineClip& clip,
                                                           const QJsonArray& streams) const
{
    QHash<int, QString> identityByTrackId;
    const QJsonObject speakerFlow = m_loadedTranscriptDoc.object().value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipFlow.value(QStringLiteral("resolved_current")).toObject();
    const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
    for (const QJsonValue& value : resolvedMap) {
        const QJsonObject resolved = resolveFaceStreamAssignmentRow(clip, streams, value.toObject());
        const int trackId = resolved.value(QStringLiteral("track_id")).toInt(-1);
        const QString identityId = resolved.value(QStringLiteral("identity_id")).toString().trimmed();
        if (trackId >= 0 && !identityId.isEmpty()) {
            identityByTrackId.insert(trackId, identityId);
        }
    }
    return identityByTrackId;
}

QVector<int> SpeakersTab::resolvedAssignedTrackIdsForSpeaker(const TimelineClip& clip,
                                                             const QJsonArray& streams,
                                                             const QString& speakerId) const
{
    QVector<int> trackIds;
    const QJsonObject speakerFlow = m_loadedTranscriptDoc.object().value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipFlow.value(QStringLiteral("resolved_current")).toObject();
    const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
    for (const QJsonValue& value : resolvedMap) {
        const QJsonObject resolved = resolveFaceStreamAssignmentRow(clip, streams, value.toObject());
        if (resolved.value(QStringLiteral("identity_id")).toString().trimmed() != speakerId) {
            continue;
        }
        const int trackId = resolved.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId >= 0 && !trackIds.contains(trackId)) {
            trackIds.push_back(trackId);
        }
    }
    return trackIds;
}

QString SpeakersTab::assignedFaceStreamPreviewTooltipHtml(const TimelineClip& clip,
                                                          const QString& speakerId) const
{
    const QFileInfo transcriptInfo(m_loadedTranscriptPath);
    const qint64 artifactRevisionMs = facestreamArtifactRevisionMsForTranscript(m_loadedTranscriptPath);
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4")
        .arg(clip.id)
        .arg(speakerId)
        .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
        .arg(artifactRevisionMs);
    const auto cached = m_avatarHoverTooltipHtmlCache.constFind(cacheKey);
    if (cached != m_avatarHoverTooltipHtmlCache.cend()) {
        return cached.value();
    }

    const QVector<QPixmap> previews = assignedFaceStreamPreviewPixmaps(clip, speakerId);
    if (previews.isEmpty()) {
        return QString();
    }

    QString html = QStringLiteral("<div style='white-space:nowrap;'>");
    int count = 0;
    for (const QPixmap& pixmap : previews) {
        if (pixmap.isNull()) {
            continue;
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        pixmap.save(&buffer, "PNG");
        html += QStringLiteral("<img width='72' height='72' style='margin:2px;border:1px solid #f4d35e;border-radius:6px;' src='data:image/png;base64,%1' />")
                    .arg(QString::fromLatin1(bytes.toBase64()));
        ++count;
        if (count >= 12) {
            break;
        }
    }
    html += QStringLiteral("</div>");
    if (count == 0) {
        return QString();
    }
    m_avatarHoverTooltipHtmlCache.insert(cacheKey, html);
    return html;
}

void SpeakersTab::showSpeakerAvatarHoverPreview(const QString& speakerId, const QPoint& globalPos)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || speakerId.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    const QString html = assignedFaceStreamPreviewTooltipHtml(*clip, speakerId);
    if (html.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    QToolTip::showText(globalPos, html, m_widgets.speakersTable);
}

void SpeakersTab::hideSpeakerAvatarHoverPreview()
{
    QToolTip::hideText();
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
        m_avatarCache.clear();
        m_avatarHoverTooltipHtmlCache.clear();
    }

    const bool canReuseLoadedDoc =
        m_loadedTranscriptDoc.isObject() &&
        m_loadedTranscriptPath == transcriptPath &&
        m_loadedClipFilePath == clip->filePath;
    if (!canReuseLoadedDoc) {
        editor::TranscriptEngine transcriptEngine;
        QJsonDocument transcriptDoc;
        if (!transcriptEngine.loadTranscriptJson(transcriptPath, &transcriptDoc)) {
            if (m_widgets.speakersInspectorClipLabel) {
                m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
            }
            if (m_widgets.speakersInspectorDetailsLabel) {
                m_widgets.speakersInspectorDetailsLabel->setText(QStringLiteral("Unable to load transcript JSON file."));
            }
            m_updating = false;
            updateSelectedSpeakerPanel();
            updateSpeakerTrackingStatusLabel();
            return;
        }
        m_loadedTranscriptPath = transcriptPath;
        m_loadedClipFilePath = clip->filePath;
        m_loadedTranscriptDoc = transcriptDoc;
    }

    if (m_widgets.speakersInspectorClipLabel) {
        m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        m_widgets.speakersInspectorDetailsLabel->setText(
            QStringLiteral("FaceStream sidecar: %1")
                .arg(facestreamSidecarExistsForClipFile(clip->filePath)
                         ? QStringLiteral("Present")
                         : QStringLiteral("Missing")));
    }

    refreshSpeakersTable(m_loadedTranscriptDoc.object(), preferredSpeakerId);
    requestRefreshFaceStreamPathsPanel();
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }

    m_updating = false;
    updateSelectedSpeakerPanel();
    updateSpeakerTrackingStatusLabel();
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
    QJsonObject continuityByClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
    const QString clipId = clip->id.trimmed();
    const QJsonObject continuityRoot = continuityByClip.value(clipId).toObject();
    const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(
        continuityRoot,
        m_loadedTranscriptDoc.object());
    const bool hasStoredPayload =
        jcut::facestream::continuityRootHasStoredPayload(continuityRoot);

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

    if (confirmDialog) {
        const auto confirmation = QMessageBox::warning(
            nullptr,
            QStringLiteral("Delete FaceStream"),
            QStringLiteral("Delete all FaceStream paths for this clip?\n\nThis cannot be undone."),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (confirmation != QMessageBox::Yes) {
            return editor::ActionResult::failure(
                QStringLiteral("canceled"),
                QStringLiteral("Delete FaceStream was canceled."));
        }
    }

    continuityByClip.remove(clipId);
    artifactRoot[QStringLiteral("continuity_facestreams_by_clip")] = continuityByClip;
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
        QJsonObject processedByClip =
            processedArtifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
        if (processedByClip.contains(clipId)) {
            processedByClip.remove(clipId);
            processedArtifactRoot[QStringLiteral("continuity_facestreams_by_clip")] = processedByClip;
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
        const QJsonObject byClip =
            artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
        const QJsonObject continuityRoot = byClip.value(clip->id.trimmed()).toObject();
        if (jcut::facestream::continuityRootHasStoredPayload(continuityRoot)) {
            return true;
        }
    }

    QJsonObject processedArtifactRoot;
    if (engine.loadFacestreamProcessedArtifact(m_loadedTranscriptPath, &processedArtifactRoot)) {
        const QJsonObject processedByClip =
            processedArtifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
        if (jcut::facestream::continuityRootHasStoredPayload(
                processedByClip.value(clip->id.trimmed()).toObject())) {
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
        const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
        continuityRoot = byClip.value(clip->id.trimmed()).toObject();
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
    if (row >= 0 && row < m_boxStreamPanelRows.size()) {
        text += QStringLiteral("Selected stream:\n");
        text += QString::fromUtf8(QJsonDocument(m_boxStreamPanelRows.at(row).toObject()).toJson(QJsonDocument::Indented));
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

void SpeakersTab::requestRefreshFaceStreamPathsPanel()
{
    if (!m_boxStreamPanelRefreshTimer) {
        refreshFaceStreamPathsPanel();
        return;
    }
    m_boxStreamPanelRefreshQueued = true;
    m_boxStreamPanelRefreshTimer->start();
}

void SpeakersTab::refreshFaceStreamPathsPanel()
{
    if (!m_widgets.speakerFaceStreamTable || m_refreshingFaceStreamPathsPanel) {
        return;
    }
    m_refreshingFaceStreamPathsPanel = true;
    struct RefreshGuard {
        bool& flag;
        ~RefreshGuard() { flag = false; }
    } guard{m_refreshingFaceStreamPathsPanel};

    QSignalBlocker tableBlocker(m_widgets.speakerFaceStreamTable);
    QSignalBlocker selectionBlocker(
        m_widgets.speakerFaceStreamTable->selectionModel());
    m_widgets.speakerFaceStreamTable->clearContents();
    m_widgets.speakerFaceStreamTable->setRowCount(0);
    if (m_widgets.speakerFaceStreamTable->columnCount() >= 5) {
        m_widgets.speakerFaceStreamTable->setHorizontalHeaderLabels(
            QStringList{
                QStringLiteral("Stream"),
                QStringLiteral("Track"),
                QStringLiteral("Assignment"),
                QStringLiteral("Range"),
                QStringLiteral("Source")
            });
    }
    m_boxStreamPanelRows = QJsonArray();
    if (m_widgets.speakerFaceStreamDetailsEdit) {
        m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
            QStringLiteral("Select a FaceStream path row to inspect full JSON."));
    }
    if (!m_loadedTranscriptDoc.isObject()) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
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
    if (refreshSignature == m_boxStreamPanelRefreshSignature) {
        return;
    }

    const QJsonArray streams = continuityStreamsForClip(*clip);
    m_boxStreamPanelRefreshSignature = refreshSignature;
    const QHash<int, QString> identityByTrackId = resolvedIdentityByTrackId(*clip, streams);

    struct StreamRow {
        QJsonObject streamObj;
        int trackId = -1;
        QString identityId;
        int64_t minFrame = -1;
        int64_t maxFrame = -1;
        QString sourceTag;
        int keyframeCount = 0;
    };
    QVector<StreamRow> panelRows;
    panelRows.reserve(streams.size());
    for (int row = 0; row < streams.size(); ++row) {
        const QJsonObject streamObj = streams.at(row).toObject();
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        int64_t minFrame = std::numeric_limits<int64_t>::max();
        int64_t maxFrame = std::numeric_limits<int64_t>::min();
        QString sourceTag;
        for (const QJsonValue& value : keyframes) {
            const QJsonObject keyframe = value.toObject();
            const int64_t frame = keyframe.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            minFrame = qMin(minFrame, frame);
            maxFrame = qMax(maxFrame, frame);
            if (sourceTag.isEmpty()) {
                sourceTag = keyframe.value(QString(kTranscriptSpeakerTrackingSourceKey)).toString().trimmed();
            }
        }
        StreamRow panelRow;
        panelRow.streamObj = streamObj;
        panelRow.trackId = trackId;
        panelRow.identityId = identityByTrackId.value(trackId).trimmed();
        panelRow.minFrame = keyframes.isEmpty() ? -1 : minFrame;
        panelRow.maxFrame = keyframes.isEmpty() ? -1 : maxFrame;
        panelRow.sourceTag = sourceTag;
        panelRow.keyframeCount = keyframes.size();
        panelRows.push_back(panelRow);
        Q_UNUSED(streamId)
    }
    std::sort(panelRows.begin(), panelRows.end(), [](const StreamRow& a, const StreamRow& b) {
        const bool aAssigned = !a.identityId.isEmpty();
        const bool bAssigned = !b.identityId.isEmpty();
        if (aAssigned != bAssigned) {
            return aAssigned && !bAssigned;
        }
        if (aAssigned && bAssigned && a.identityId != b.identityId) {
            return a.identityId.localeAwareCompare(b.identityId) < 0;
        }
        return a.trackId < b.trackId;
    });

    int assignedCount = 0;
    int unassignedCount = 0;
    for (const StreamRow& row : std::as_const(panelRows)) {
        if (row.identityId.isEmpty()) {
            ++unassignedCount;
        } else {
            ++assignedCount;
        }
        m_boxStreamPanelRows.push_back(row.streamObj);
    }

    m_widgets.speakerFaceStreamTable->setRowCount(panelRows.size());
    for (int row = 0; row < panelRows.size(); ++row) {
        const StreamRow& panelRow = panelRows.at(row);
        const QJsonObject streamObj = panelRow.streamObj;
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const int trackId = panelRow.trackId;
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        const QString rangeText = keyframes.isEmpty()
            ? QStringLiteral("-")
            : QStringLiteral("%1..%2").arg(panelRow.minFrame).arg(panelRow.maxFrame);
        auto* streamItem = new QTableWidgetItem(streamId.isEmpty() ? QStringLiteral("—") : streamId);
        streamItem->setData(Qt::UserRole + 1, row);
        const qlonglong seekFrame = keyframes.isEmpty()
            ? static_cast<qlonglong>(-1)
            : static_cast<qlonglong>(panelRow.minFrame);
        streamItem->setData(Qt::UserRole + 2, QVariant(seekFrame));
        auto* trackItem = new QTableWidgetItem(trackId >= 0 ? QString::number(trackId) : QStringLiteral("—"));
        const bool assigned = !panelRow.identityId.isEmpty();
        auto* countItem = new QTableWidgetItem(
            assigned ? panelRow.identityId : QStringLiteral("Unassigned"));
        auto* rangeItem = new QTableWidgetItem(rangeText);
        auto* sourceItem = new QTableWidgetItem(
            panelRow.sourceTag.isEmpty() ? QStringLiteral("continuity_track_v1") : panelRow.sourceTag);
        countItem->setToolTip(
            assigned
                ? QStringLiteral("Assigned to speaker identity: %1").arg(panelRow.identityId)
                : QStringLiteral("No speaker identity assignment yet."));
        rangeItem->setToolTip(QStringLiteral("Keyframes: %1").arg(panelRow.keyframeCount));
        if (!assigned) {
            const QColor bg(QStringLiteral("#3a2a2a"));
            streamItem->setBackground(bg);
            trackItem->setBackground(bg);
            countItem->setBackground(bg);
            rangeItem->setBackground(bg);
            sourceItem->setBackground(bg);
        }
        m_widgets.speakerFaceStreamTable->setItem(row, 0, streamItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 1, trackItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 2, countItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 3, rangeItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 4, sourceItem);
    }
    if (streams.isEmpty() && m_widgets.speakerFaceStreamDetailsEdit) {
        m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
            QStringLiteral("No FaceStream paths found for this clip. Run JCut DNN FaceStream Generator first."));
    } else if (!streams.isEmpty()) {
        if (m_widgets.speakerFaceStreamDetailsEdit) {
            m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
                QStringLiteral("Track assignment summary\n\nAssigned: %1\nUnassigned: %2\nTotal: %3\n\nSelect a row to inspect full JSON.")
                    .arg(assignedCount)
                    .arg(unassignedCount)
                    .arg(panelRows.size()));
        }
        m_widgets.speakerFaceStreamTable->setCurrentCell(0, 0);
    }
}

void SpeakersTab::refreshSpeakersTable(const QJsonObject& transcriptRoot,
                                       const QString& preferredSpeakerId)
{
    if (!m_widgets.speakersTable) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
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

    QStringList ids = speakerIds.values();
    std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });
    const QString selectionToRestore =
        preferredSpeakerId.isEmpty() ? selectedSpeakerId() : preferredSpeakerId;

    const QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();

    QSignalBlocker blocker(m_widgets.speakersTable);
    m_widgets.speakersTable->setRowCount(ids.size());
    for (int row = 0; row < ids.size(); ++row) {
        const QString id = ids.at(row);
        const QJsonObject profileJson = profiles.value(id).toObject();
        const SpeakerProfile speakerProfile = speakerProfileFromJson(id, profileJson);
        const QString name = speakerProfile.name.isEmpty() ? id : speakerProfile.name;
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
        avatarItem->setIcon(QIcon(speakerAvatarForRow(*clip, transcriptRoot, profileJson, id)));
        const uint hueHash = qHash(id);
        const QColor speakerHueTint = QColor::fromHsv(static_cast<int>(hueHash % 360), 160, 92, 105);
        avatarItem->setBackground(QBrush(speakerHueTint));
        avatarItem->setToolTip(
            QStringLiteral("Primary avatar. Unset by default. Click avatar and square-select in Preview to set."));
        avatarItem->setTextAlignment(Qt::AlignCenter);
        avatarItem->setSizeHint(QSize(32, 32));

        auto* idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        idItem->setData(Qt::UserRole, id);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, id);
        nameItem->setData(Qt::UserRole + 10, keyframeCount > 0);
        nameItem->setToolTip(QStringLiteral("Speaker: %1\nOrganization: %2\nSummary: %3")
                                 .arg(name,
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
        m_widgets.speakersTable->setItem(row, 0, avatarItem);
        m_widgets.speakersTable->setItem(row, 1, idItem);
        m_widgets.speakersTable->setItem(row, 2, nameItem);
        m_widgets.speakersTable->setItem(row, 3, xItem);
        m_widgets.speakersTable->setItem(row, 4, yItem);
        m_widgets.speakersTable->setItem(row, 5, trackingModeItem);
        m_widgets.speakersTable->setRowHeight(row, 34);
    }

    bool restoredSelection = false;
    if (!selectionToRestore.isEmpty()) {
        for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
            QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
            if (!idItem) {
                continue;
            }
            const QString rowSpeakerId = idItem->data(Qt::UserRole).toString().trimmed();
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
        QTableWidgetItem* idItem = m_widgets.speakersTable->item(0, 1);
        if (idItem) {
            m_widgets.speakersTable->setCurrentCell(0, 1);
            m_widgets.speakersTable->selectRow(0);
            m_lastSelectedSpeakerIdHint = idItem->data(Qt::UserRole).toString().trimmed();
        }
    }
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
    QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
    if (!idItem) {
        return QString();
    }
    return idItem->data(Qt::UserRole).toString().trimmed();
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

QPixmap SpeakersTab::placeholderSpeakerAvatar(const QString& speakerId) const
{
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#243447")));
    painter.drawEllipse(QRectF(1.0, 1.0, 30.0, 30.0));
    painter.setPen(QColor(QStringLiteral("#d8e6f5")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(10);
    painter.setFont(font);
    const QString fallback = speakerId.trimmed().isEmpty() ? QStringLiteral("?") : speakerId.left(1).toUpper();
    painter.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, fallback);
    return QPixmap::fromImage(image);
}

QPixmap SpeakersTab::unsetSpeakerAvatar(int size) const
{
    const int iconSize = qMax(16, size);
    QImage image(iconSize, iconSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF border(1.0, 1.0, iconSize - 2.0, iconSize - 2.0);
    painter.setPen(QPen(QColor(QStringLiteral("#5a6d82")), 1.0, Qt::DashLine));
    painter.setBrush(QColor(QStringLiteral("#1a2533")));
    painter.drawRoundedRect(border, 4.0, 4.0);
    painter.setPen(QColor(QStringLiteral("#9fb3c8")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(qMax(7, iconSize / 4));
    painter.setFont(font);
    painter.drawText(QRectF(0.0, 0.0, iconSize, iconSize), Qt::AlignCenter, QStringLiteral("?"));
    return QPixmap::fromImage(image);
}

QPixmap SpeakersTab::speakerAvatarForRow(const TimelineClip& clip,
                                         const QJsonObject& transcriptRoot,
                                         const QJsonObject& profile,
                                         const QString& speakerId)
{
    const QJsonArray faceRefs = speakerFaceRefs(profile);
    for (int i = faceRefs.size() - 1; i >= 0; --i) {
        const QJsonObject previewKeyframe = previewKeyframeFromSpeakerFaceRef(faceRefs.at(i).toObject());
        if (!previewKeyframe.isEmpty()) {
            return faceStreamPreviewAvatar(clip, speakerId, previewKeyframe, 32);
        }
    }
    const QJsonArray streams = continuityStreamsForClip(clip);
    const QVector<int> assignedTrackIds =
        resolvedAssignedTrackIdsForSpeaker(clip, streams, speakerId);
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (!assignedTrackIds.contains(trackId)) {
            continue;
        }
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (keyframes.isEmpty()) {
            continue;
        }
        return faceStreamPreviewAvatar(clip, speakerId, keyframes.first().toObject(), 32);
    }
    return unsetSpeakerAvatar(32);
}

QPixmap SpeakersTab::speakerReferenceAvatar(const TimelineClip& clip,
                                            const QString& speakerId,
                                            const QJsonObject& refObj,
                                            int size)
{
    if (refObj.isEmpty()) {
        Q_UNUSED(speakerId);
        return unsetSpeakerAvatar(size);
    }
    const int avatarSize = qMax(16, size);

    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    qreal locX = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    qreal locY = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);

    const QString cacheKey = QStringLiteral("ref|%1|%2|%3|%4|%5|%6")
        .arg(m_loadedTranscriptPath)
        .arg(speakerId)
        .arg(sourceFrame30)
        .arg(static_cast<int>(std::round(locX * 1000.0)))
        .arg(static_cast<int>(std::round(locY * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxSizeNorm) * 1000.0))) +
        QStringLiteral("|s=%1").arg(avatarSize);
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);

    editor::DecoderContext ctx(mediaPath);
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    if (ctx.initialize()) {
        const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
        const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        if (!image.isNull() && image.width() > 0 && image.height() > 0) {
            const int width = image.width();
            const int height = image.height();
            const int minSide = qMin(width, height);
            int side = qMax(40, minSide / 3);
            if (boxSizeNorm > 0.0) {
                side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
            }
            const int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
            const int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
            const int left = qBound(0, cx - (side / 2), qMax(0, width - side));
            const int top = qBound(0, cy - (side / 2), qMax(0, height - side));
            QImage crop = image.copy(QRect(left, top, qMin(side, width), qMin(side, height)))
                              .scaled(avatarSize, avatarSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (!crop.isNull()) {
                QImage rounded(avatarSize, avatarSize, QImage::Format_ARGB32_Premultiplied);
                rounded.fill(Qt::transparent);
                QPainter painter(&rounded);
                painter.setRenderHint(QPainter::Antialiasing, true);
                QPainterPath path;
                path.addEllipse(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0);
                painter.setClipPath(path);
                painter.drawImage(QRect(0, 0, avatarSize, avatarSize), crop);
                painter.setClipping(false);
                painter.setPen(QPen(QColor(QStringLiteral("#8dbbe4")), 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0));
                avatar = QPixmap::fromImage(rounded);
            }
        }
    }

    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QPixmap SpeakersTab::referenceFullFramePreview(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& refObj,
                                               QSize targetSize)
{
    Q_UNUSED(speakerId);
    const int outputW = qMax(640, targetSize.width());
    const int outputH = qMax(360, targetSize.height());
    QImage canvas(outputW, outputH, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(QStringLiteral("#111820")));

    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal locX = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal locY = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));

    const QString mediaPath = interactivePreviewMediaPathForClip(clip);
    editor::DecoderContext ctx(mediaPath);
    if (!ctx.initialize()) {
        return QPixmap::fromImage(canvas);
    }

    const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
    const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return QPixmap::fromImage(canvas);
    }

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QImage display = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QSize scaled = display.size().scaled(canvas.size(), Qt::KeepAspectRatio);
    const QRect drawRect(
        (canvas.width() - scaled.width()) / 2,
        (canvas.height() - scaled.height()) / 2,
        scaled.width(),
        scaled.height());
    painter.drawImage(drawRect, display);

    const qreal refPxX = drawRect.left() + (locX * drawRect.width());
    const qreal refPxY = drawRect.top() + (locY * drawRect.height());
    const int minSide = qMin(drawRect.width(), drawRect.height());
    int boxSide = qMax(48, minSide / 4);
    if (boxSizeNorm > 0.0) {
        boxSide = qBound(48, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
    }
    const QRectF boxRect(refPxX - (boxSide / 2.0), refPxY - (boxSide / 2.0), boxSide, boxSide);

    painter.setPen(QPen(QColor(QStringLiteral("#ffb347")), 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(boxRect);
    painter.setPen(QPen(QColor(QStringLiteral("#6fd6ff")), 3.0));
    painter.drawEllipse(QPointF(refPxX, refPxY), 5.0, 5.0);
    painter.setPen(QPen(QColor(QStringLiteral("#6fd6ff")), 1.0));
    painter.drawLine(QPointF(refPxX - 14.0, refPxY), QPointF(refPxX + 14.0, refPxY));
    painter.drawLine(QPointF(refPxX, refPxY - 14.0), QPointF(refPxX, refPxY + 14.0));

    return QPixmap::fromImage(canvas);
}

bool SpeakersTab::openReferencePreviewWindow(int referenceIndex)
{
    if (referenceIndex != 1 && referenceIndex != 2) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    QString speakerId;
    QJsonObject refObj;
    if (!selectedSpeakerReferenceObject(referenceIndex, &speakerId, &refObj)) {
        if (activeCutMutable()) {
            onSpeakerPrecropFacesClicked();
            return true;
        }
        return false;
    }

    QDialog* dialog = new QDialog();
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setWindowTitle(QStringLiteral("Ref %1 Source Preview").arg(referenceIndex));
    dialog->resize(980, 720);

    QVBoxLayout* root = new QVBoxLayout(dialog);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel* imageLabel = new QLabel(dialog);
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(640, 360);
    imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageLabel->setStyleSheet(QStringLiteral("background:#0d141d;border:1px solid #263748;border-radius:8px;"));
    root->addWidget(imageLabel, 1);

    QLabel* detailLabel = new QLabel(dialog);
    detailLabel->setWordWrap(true);
    detailLabel->setStyleSheet(QStringLiteral("color:#9eb6cf;"));
    root->addWidget(detailLabel);

    QFrame* controlsFrame = new QFrame(dialog);
    controlsFrame->setStyleSheet(QStringLiteral(
        "QFrame{background:#101924;border:1px solid #2a3f53;border-radius:8px;}"));
    QVBoxLayout* controlsLayout = new QVBoxLayout(controlsFrame);
    controlsLayout->setContentsMargins(8, 8, 8, 8);
    controlsLayout->setSpacing(6);

    QLabel* sentenceLabel = new QLabel(dialog);
    sentenceLabel->setWordWrap(true);
    sentenceLabel->setStyleSheet(QStringLiteral("color:#d5e5f6;"));
    controlsLayout->addWidget(sentenceLabel);

    QHBoxLayout* sentenceButtons = new QHBoxLayout();
    QPushButton* prevButton = new QPushButton(QStringLiteral("Prev Sentence"), dialog);
    QPushButton* nextButton = new QPushButton(QStringLiteral("Next Sentence"), dialog);
    QPushButton* randomButton = new QPushButton(QStringLiteral("Random Sentence"), dialog);
    sentenceButtons->addWidget(prevButton);
    sentenceButtons->addWidget(nextButton);
    sentenceButtons->addWidget(randomButton);
    controlsLayout->addLayout(sentenceButtons);

    QHBoxLayout* refButtons = new QHBoxLayout();
    QPushButton* pickButton = new QPushButton(QStringLiteral("Pick Ref %1 (Shift+Drag)").arg(referenceIndex), dialog);
    QPushButton* setButton = new QPushButton(QStringLiteral("Set Ref %1 @ Current Frame").arg(referenceIndex), dialog);
    refButtons->addWidget(pickButton);
    refButtons->addWidget(setButton);
    controlsLayout->addLayout(refButtons);
    root->addWidget(controlsFrame);

    auto refreshDialog = [this, imageLabel, detailLabel, sentenceLabel, referenceIndex]() {
        const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        QString refSpeakerId;
        QJsonObject latestRef;
        if (!selectedClip || !selectedSpeakerReferenceObject(referenceIndex, &refSpeakerId, &latestRef)) {
            imageLabel->setPixmap(QPixmap());
            detailLabel->setText(QStringLiteral("Reference unavailable."));
            sentenceLabel->setText(QStringLiteral("Sentence context unavailable."));
            return;
        }
        imageLabel->setPixmap(referenceFullFramePreview(*selectedClip, refSpeakerId, latestRef, QSize(1100, 620)));
        const int64_t frame30 =
            qMax<int64_t>(0, latestRef.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
        const qreal xNorm =
            qBound<qreal>(0.0, latestRef.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const qreal yNorm =
            qBound<qreal>(0.0, latestRef.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
        detailLabel->setText(
            QStringLiteral("Untransformed clip frame %1 | Speaker %2 | x=%3 y=%4")
                .arg(frame30)
                .arg(refSpeakerId)
                .arg(QString::number(xNorm, 'f', 3))
                .arg(QString::number(yNorm, 'f', 3)));
        sentenceLabel->setText(currentSpeakerSentenceAtCurrentFrame(refSpeakerId));
    };

    connect(prevButton, &QPushButton::clicked, dialog, [this, refreshDialog, speakerId]() {
        navigateSpeakerSentence(speakerId, SentenceNavAction::Previous);
        refreshDialog();
    });
    connect(nextButton, &QPushButton::clicked, dialog, [this, refreshDialog, speakerId]() {
        navigateSpeakerSentence(speakerId, SentenceNavAction::Next);
        refreshDialog();
    });
    connect(randomButton, &QPushButton::clicked, dialog, [this, refreshDialog, speakerId]() {
        navigateSpeakerSentence(speakerId, SentenceNavAction::Random);
        refreshDialog();
    });
    connect(pickButton, &QPushButton::clicked, dialog, [this, referenceIndex]() {
        const QString id = selectedSpeakerId();
        if (!id.isEmpty()) {
            armReferencePickForSpeaker(id, referenceIndex);
        }
    });
    connect(setButton, &QPushButton::clicked, dialog, [this, refreshDialog, referenceIndex]() {
        const QString id = selectedSpeakerId();
        if (id.isEmpty()) {
            return;
        }
        saveSpeakerTrackingReference(id, referenceIndex);
        refreshDialog();
    });

    refreshDialog();
    dialog->show();
    return true;
}

QString SpeakersTab::speakerTrackingSummary(const QJsonObject& profile) const
{
    const QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return QStringLiteral("Subtitle Face Tracking: Off");
    }
    const QString mode = tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
    const bool enabled = transcriptTrackingEnabled(tracking);
    const QString autoState = tracking.value(QString(kTranscriptSpeakerTrackingAutoStateKey)).toString();
    const int keyframeCount = tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();
    QString summary = QStringLiteral("Subtitle Face Tracking: %1 (%2)")
        .arg(enabled ? QStringLiteral("On") : QStringLiteral("Off"))
        .arg(mode);
    if (keyframeCount > 0) {
        summary += QStringLiteral(" | Keys=%1").arg(keyframeCount);
    }
    if (!autoState.isEmpty()) {
        summary += QStringLiteral(" | Auto=%1").arg(autoState);
    }
    return summary;
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
        m_widgets.speakerRefsChipLabel->setText(QStringLiteral("FaceStreams: 0"));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        m_widgets.speakerPointstreamChipLabel->setText(QStringLiteral("FaceStream: None"));
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setText(QStringLiteral("Tracking: OFF"));
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
            QStringLiteral("FaceStreams: %1").arg(assignedFaceStreamCount));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        if (!hasClipWideFaceStream) {
            m_widgets.speakerPointstreamChipLabel->setText(
                QStringLiteral("FaceStream: MISSING (All Speakers)"));
        } else {
            m_widgets.speakerPointstreamChipLabel->setText(
                QStringLiteral("FaceStream: ClipWide Ready"));
        }
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setText(
            trackingEnabled ? QStringLiteral("Tracking: ON") : QStringLiteral("Tracking: OFF"));
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
        QStringLiteral("Assigned FaceStreams: %1 | FaceStream: %2 | Tracking: %3 | Face Stabilize: %4")
            .arg(assignedFaceStreamCount)
            .arg(QStringLiteral("ClipWide Ready"))
            .arg(trackingEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg((selectedClip && selectedClip->speakerFramingEnabled) ? QStringLiteral("ON") : QStringLiteral("OFF")));
}


bool SpeakersTab::seekToSpeakerSegmentRelative(const QString& speakerId, int direction)
{
    if (speakerId.isEmpty() || direction == 0 || !m_loadedTranscriptDoc.isObject() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t currentTimeline = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const int64_t currentSourceFrame = qMax<int64_t>(0, clip->sourceInFrame + (currentTimeline - clip->startFrame));
    QVector<int64_t> speakerFrames = speakerSourceFrames(m_loadedTranscriptDoc.object(), speakerId);
    if (speakerFrames.isEmpty()) {
        return false;
    }

    int64_t chosenSource = -1;
    int chosenIndex = -1;
    if (direction > 0) {
        for (int i = 0; i < speakerFrames.size(); ++i) {
            if (speakerFrames.at(i) > currentSourceFrame) {
                chosenIndex = i;
                break;
            }
        }
        if (chosenIndex < 0) {
            chosenIndex = 0;
        }
    } else {
        for (int i = speakerFrames.size() - 1; i >= 0; --i) {
            if (speakerFrames.at(i) < currentSourceFrame) {
                chosenIndex = i;
                break;
            }
        }
        if (chosenIndex < 0) {
            chosenIndex = speakerFrames.size() - 1;
        }
    }

    chosenSource = speakerFrames.at(chosenIndex);
    int64_t timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);

    // Ensure button presses visibly move when multiple segments exist.
    if (timelineFrame == currentTimeline && speakerFrames.size() > 1) {
        if (direction > 0) {
            chosenIndex = (chosenIndex + 1) % speakerFrames.size();
        } else {
            chosenIndex = (chosenIndex - 1 + speakerFrames.size()) % speakerFrames.size();
        }
        chosenSource = speakerFrames.at(chosenIndex);
        timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
        timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    }

    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::seekToSpeakerRandomSentence(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    const QVector<int64_t> speakerFrames = speakerSourceFrames(m_loadedTranscriptDoc.object(), speakerId);
    if (speakerFrames.isEmpty()) {
        return false;
    }
    const int idx = QRandomGenerator::global()->bounded(speakerFrames.size());
    const int64_t chosenSource = speakerFrames.at(idx);
    int64_t timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::navigateSpeakerSentence(const QString& speakerId, SentenceNavAction action)
{
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return false;
    }
    m_lastSelectedSpeakerIdHint = speakerId;
    switch (action) {
    case SentenceNavAction::Previous:
        return seekToSpeakerSegmentRelative(speakerId, -1);
    case SentenceNavAction::Next:
        return seekToSpeakerSegmentRelative(speakerId, +1);
    case SentenceNavAction::Random:
        return seekToSpeakerRandomSentence(speakerId);
    }
    return false;
}

void SpeakersTab::onSpeakerPreviousSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Previous);
}

void SpeakersTab::onSpeakerNextSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Next);
}

void SpeakersTab::onSpeakerRandomSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Random);
}

void SpeakersTab::onSpeakerClearReferencesClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!clearSpeakerTrackingReferences(speakerId)) {
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
