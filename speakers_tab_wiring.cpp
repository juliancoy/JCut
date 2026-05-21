#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speakers_table.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QListWidget>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QWheelEvent>
#include <QJsonDocument>

void SpeakersTab::wire()
{
    if (!m_faceStreamPanelRefreshTimer) {
        m_faceStreamPanelRefreshTimer = new QTimer(this);
        m_faceStreamPanelRefreshTimer->setSingleShot(true);
        m_faceStreamPanelRefreshTimer->setInterval(40);
        connect(m_faceStreamPanelRefreshTimer, &QTimer::timeout, this, [this]() {
            m_faceStreamPanelRefreshQueued = false;
            refreshFaceStreamPathsPanel();
        });
    }
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_widgets.speakersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_widgets.speakersTable->setItemDelegateForColumn(
            1, new SpeakerNameItemDelegate(m_widgets.speakersTable));
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
    if (m_widgets.speakerShowContiguousSectionsCheckBox) {
        connect(m_widgets.speakerShowContiguousSectionsCheckBox,
                &QCheckBox::toggled,
                this,
                [this]() {
                    syncSpeakerListMode();
                    if (m_widgets.speakerShowContiguousSectionsCheckBox->isChecked() &&
                        m_transcriptSession.hasObjectDocument()) {
                        refreshSpeakerSectionsTable(m_transcriptSession.rootObject());
                    }
                });
    }
    if (m_widgets.speakerSectionsTable) {
        connect(m_widgets.speakerSectionsTable,
                &QTableWidget::itemSelectionChanged,
                this,
                [this]() {
                    const int row =
                        m_widgets.speakerSectionsTable ? m_widgets.speakerSectionsTable->currentRow() : -1;
                    if (row < 0) {
                        return;
                    }
                    QTableWidgetItem* speakerItem = m_widgets.speakerSectionsTable->item(row, 1);
                    if (!speakerItem) {
                        return;
                    }
                    const QString speakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
                    if (speakerId.isEmpty()) {
                        return;
                    }
                    selectSpeakerRowById(speakerId);
                    m_lastSelectedSpeakerIdHint = speakerId;
                    updateSpeakerTrackingStatusLabel();
                    updateSelectedSpeakerPanel();
                    const int64_t timelineFrame = speakerItem->data(Qt::UserRole + 1).toLongLong();
                    if (timelineFrame >= 0 && m_deps.seekToTimelineFrame) {
                        m_deps.seekToTimelineFrame(timelineFrame);
                    }
                });
    }
    if (m_widgets.speakerFaceStreamTable) {
        m_widgets.speakerFaceStreamTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.speakerFaceStreamTable,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::onSpeakerFaceStreamTableContextMenuRequested);
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
                if (rowIndex >= 0 && rowIndex < m_faceStreamPanelRows.size()) {
                    const QJsonObject streamObj = m_faceStreamPanelRows.at(rowIndex).toObject();
                    streamJson = QString::fromUtf8(QJsonDocument(streamObj).toJson(QJsonDocument::Indented));
                }
            }
            m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
                streamJson.isEmpty()
                    ? QStringLiteral("No stream payload available.")
                    : streamJson);
        });
    }
    if (m_widgets.speakerRawDetectionTable) {
        connect(m_widgets.speakerRawDetectionTable, &QTableWidget::itemSelectionChanged, this, [this]() {
            if (!m_widgets.speakerRawDetectionTable || !m_widgets.speakerRawDetectionDetailsEdit) {
                return;
            }
            const int row = m_widgets.speakerRawDetectionTable->currentRow();
            if (row < 0) {
                m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                    QStringLiteral("Select a detection row to inspect full JSON."));
                return;
            }
            QTableWidgetItem* indexItem = m_widgets.speakerRawDetectionTable->item(row, 0);
            QString detectionJson;
            if (indexItem) {
                const QString serialized = indexItem->data(Qt::UserRole + 1).toString();
                detectionJson = serialized;
            }
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                detectionJson.isEmpty()
                    ? QStringLiteral("No detection payload available.")
                    : detectionJson);
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
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("GENERATE CONTINUITY TRACKS"));
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
            QStringLiteral("Run raw face detection and continuity-track generation for the active transcript scope."));
        connect(m_widgets.speakerRunAutoTrackButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRunAutoTrackClicked);
    }
    if (m_widgets.speakerViewFacestreamButton) {
        m_widgets.speakerViewFacestreamButton->setToolTip(
            QStringLiteral("Open the selected continuity-track payload and the latest generated artifact paths."));
        connect(m_widgets.speakerViewFacestreamButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerViewFaceStreamClicked);
    }
    if (m_widgets.speakerFacestreamSettingsButton) {
        m_widgets.speakerFacestreamSettingsButton->setToolTip(
            QStringLiteral("Open continuity-track rebuild and smoothing tools."));
        connect(m_widgets.speakerFacestreamSettingsButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerFaceStreamSettingsClicked);
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setToolTip(
            QStringLiteral("Click to toggle Speaker Tracking for the selected speaker."));
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
            QStringLiteral("Open a quick guide for detections, continuity tracks, and speaker assignment."));
        connect(m_widgets.speakerGuideButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerGuideClicked);
    }
    if (m_widgets.speakerPrecropFacesButton) {
        m_widgets.speakerPrecropFacesButton->setToolTip(
            QStringLiteral("Assign the selected continuity tracks from the playhead list to the current speaker."));
        connect(m_widgets.speakerPrecropFacesButton, &QPushButton::clicked, this, [this]() {
            // Keep REST/UI click handlers from blocking while the inline add-tracks action runs.
            QTimer::singleShot(0, this, &SpeakersTab::onSpeakerPrecropFacesClicked);
        });
    }
    if (m_widgets.speakerPlayheadFaceStreamsList) {
        connect(m_widgets.speakerPlayheadFaceStreamsList, &QListWidget::itemSelectionChanged, this, [this]() {
            const bool hasSpeaker = !selectedSpeakerId().trimmed().isEmpty();
            const bool hasSelection =
                m_widgets.speakerPlayheadFaceStreamsList &&
                !m_widgets.speakerPlayheadFaceStreamsList->selectedItems().isEmpty();
            if (m_widgets.speakerPrecropFacesButton) {
                const bool playheadListVisible =
                    !m_widgets.speakerShowPlayheadFaceStreamsCheckBox ||
                    m_widgets.speakerShowPlayheadFaceStreamsCheckBox->isChecked();
                m_widgets.speakerPrecropFacesButton->setEnabled(
                    activeCutMutable() && playheadListVisible && hasSpeaker && hasSelection);
            }
        });
    }
    if (m_widgets.speakerShowPlayheadFaceStreamsCheckBox) {
        connect(m_widgets.speakerShowPlayheadFaceStreamsCheckBox, &QCheckBox::toggled, this, [this]() {
            updatePlayheadTrackCandidatesVisibility();
        });
    }
    if (m_widgets.speakerFaceStreamOverlaySourceCombo) {
        connect(m_widgets.speakerFaceStreamOverlaySourceCombo,
                &QComboBox::currentIndexChanged,
                this,
                [this](int) {
                    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
                    const QString speakerId = selectedSpeakerId();
                    if (clip && !speakerId.trimmed().isEmpty()) {
                        refreshPlayheadTrackCandidatesList(*clip, speakerId);
                        updatePlayheadTrackCandidatesVisibility();
                        updateSpeakerTrackingStatusLabel();
                    }
                });
    }
    if (m_widgets.selectedSpeakerFaceStreamsList) {
        m_widgets.selectedSpeakerFaceStreamsList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.selectedSpeakerFaceStreamsList,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::showSelectedSpeakerAssignedTracksContextMenu);
        auto* deassignShortcut = new QAction(m_widgets.selectedSpeakerFaceStreamsList);
        deassignShortcut->setShortcut(QKeySequence::Delete);
        deassignShortcut->setShortcutContext(Qt::WidgetShortcut);
        connect(deassignShortcut, &QAction::triggered, this, [this]() {
            deassignSelectedSpeakerAssignedTracks();
        });
        m_widgets.selectedSpeakerFaceStreamsList->addAction(deassignShortcut);
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
            QStringLiteral("Click: open Ref 1 preview/identity assignment. Shift+Drag: adjust crop."));
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->installEventFilter(this);
        m_widgets.selectedSpeakerRef2ImageLabel->setCursor(Qt::PointingHandCursor);
        m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(
            QStringLiteral("Click: open Ref 2 preview/identity assignment. Shift+Drag: adjust crop."));
    }
    updatePlayheadTrackCandidatesVisibility();
    syncSpeakerListMode();
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
