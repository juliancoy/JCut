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

namespace {
bool shouldUseSynchronousSpeakerTranscriptIo()
{
    if (qEnvironmentVariableIntValue("JCUT_SYNC_TRANSCRIPT_IO") == 1) {
        return true;
    }
    const QString appName = QCoreApplication::applicationName().trimmed().toLower();
    return appName.startsWith(QStringLiteral("test_")) || appName.contains(QStringLiteral("qtest"));
}
}

SpeakersTab::SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : TableTabBase(deps, parent)
    , m_widgets(widgets)
    , m_speakerDeps(deps)
{
    connect(&m_transcriptLoadWatcher, &QFutureWatcher<TranscriptLoadResult>::finished, this, [this]() {
        const TranscriptLoadResult result = m_transcriptLoadWatcher.result();
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
    connect(&m_transcriptSaveWatcher, &QFutureWatcher<TranscriptSaveResult>::finished, this, [this]() {
        const TranscriptSaveResult result = m_transcriptSaveWatcher.result();
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
                TranscriptSaveResult next;
                next.transcriptPath = path;
                next.revision = revision;
                editor::TranscriptEngine engine;
                next.ok = engine.saveTranscriptJson(path, doc);
                if (!next.ok) {
                    next.error = QStringLiteral("saveTranscriptJson returned false.");
                }
                return next;
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
    if (shouldUseSynchronousSpeakerTranscriptIo()) {
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
        TranscriptSaveResult result;
        result.transcriptPath = path;
        result.revision = revision;
        editor::TranscriptEngine engine;
        result.ok = engine.saveTranscriptJson(path, doc);
        if (!result.ok) {
            result.error = QStringLiteral("saveTranscriptJson returned false.");
        }
        return result;
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
    if (shouldUseSynchronousSpeakerTranscriptIo()) {
        QJsonDocument transcriptDoc;
        if (loadTranscriptJsonCached(transcriptPath, &transcriptDoc)) {
            m_loadedTranscriptDoc = transcriptDoc;
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
                m_widgets.speakersInspectorDetailsLabel->setText(QStringLiteral("Unable to load transcript JSON file."));
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
        TranscriptLoadResult result;
        result.clipFilePath = clipFilePath;
        result.transcriptPath = transcriptPath;
        result.ok = loadTranscriptJsonCached(transcriptPath, &result.document);
        if (!result.ok) {
            result.error = QStringLiteral("Unable to load transcript JSON file.");
        }
        return result;
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
                        m_loadedTranscriptDoc.isObject()) {
                        refreshSpeakerSectionsTable(m_loadedTranscriptDoc.object());
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
            QStringLiteral("Extract representative identity crops per continuity track and assign them to transcript speakers."));
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
            QStringLiteral("Click: open Ref 1 preview/identity assignment. Shift+Drag: adjust crop."));
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->installEventFilter(this);
        m_widgets.selectedSpeakerRef2ImageLabel->setCursor(Qt::PointingHandCursor);
        m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(
            QStringLiteral("Click: open Ref 2 preview/identity assignment. Shift+Drag: adjust crop."));
    }
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
    return faceStreamPreviewAvatarWithDecoder(
        clip, speakerId, keyframeObj, size, nullptr, nullptr, resolvedSourceFps(clip));
}

QPixmap SpeakersTab::faceStreamPreviewAvatarWithDecoder(const TimelineClip& clip,
                                                        const QString& speakerId,
                                                        const QJsonObject& keyframeObj,
                                                        int size,
                                                        editor::DecoderContext* decoderCtx,
                                                        QHash<int64_t, QImage>* frameImageCache,
                                                        qreal sourceFps) const
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

    const qreal effectiveSourceFps = sourceFps > 0.0 ? sourceFps : resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * effectiveSourceFps)));
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    QImage image;
    if (frameImageCache) {
        const auto cachedFrame = frameImageCache->constFind(decodeFrame);
        if (cachedFrame != frameImageCache->cend()) {
            image = cachedFrame.value();
        }
    }
    if (image.isNull()) {
        std::unique_ptr<editor::DecoderContext> localDecoder;
        editor::DecoderContext* activeDecoder = decoderCtx;
        if (!activeDecoder) {
            const QString mediaPath = interactivePreviewMediaPathForClip(clip);
            localDecoder = std::make_unique<editor::DecoderContext>(mediaPath);
            if (!localDecoder->initialize()) {
                activeDecoder = nullptr;
            } else {
                activeDecoder = localDecoder.get();
            }
        }
        if (activeDecoder) {
            const editor::FrameHandle frame = activeDecoder->decodeFrame(decodeFrame);
            image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
            if (frameImageCache && !image.isNull()) {
                frameImageCache->insert(decodeFrame, image);
            }
        }
    }
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
    const QString cacheKey = m_loadedTranscriptPath + QLatin1Char('\n') + clip.id.trimmed();
    const auto cached = m_continuityStreamsCache.constFind(cacheKey);
    if (cached != m_continuityStreamsCache.cend()) {
        return cached.value();
    }

    QJsonArray streams;
    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    if (transcriptEngine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
        streams = jcut::facestream::continuityStreamsForRoot(
            continuityRoot,
            m_loadedTranscriptDoc.object());
        if (!streams.isEmpty()) {
            m_continuityStreamsCache.insert(cacheKey, streams);
            return streams;
        }
    }
    const QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject speakerFlow = root.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject continuityRoot = clipFlow.value(QStringLiteral("continuity_facestreams")).toObject();
    streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    m_continuityStreamsCache.insert(cacheKey, streams);
    return streams;
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
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString(),
                &frameDomain)) {
            continue;
        }
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            const int64_t frame =
                keyframeObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            streamFrameMin = qMin<int64_t>(streamFrameMin, frame);
            streamFrameMax = qMax<int64_t>(streamFrameMax, frame);
        }

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
        m_widgets.speakerSectionsTable->setCurrentCell(0, 0);
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

void SpeakersTab::requestRefreshFaceStreamPathsPanel()
{
    if (!m_faceStreamPanelRefreshTimer) {
        refreshFaceStreamPathsPanel();
        return;
    }
    m_faceStreamPanelRefreshQueued = true;
    m_faceStreamPanelRefreshTimer->start();
}

void SpeakersTab::refreshFaceStreamPathsPanel()
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    if (!m_widgets.speakerFaceStreamTable || m_refreshingFaceStreamPathsPanel) {
        m_lastFaceStreamPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxFaceStreamPanelRefreshDurationMs =
            qMax(m_maxFaceStreamPanelRefreshDurationMs, m_lastFaceStreamPanelRefreshDurationMs);
        return;
    }
    m_refreshingFaceStreamPathsPanel = true;
    struct RefreshGuard {
        bool& flag;
        qint64* lastDurationMs = nullptr;
        qint64* maxDurationMs = nullptr;
        QElapsedTimer* timer = nullptr;
        ~RefreshGuard() {
            flag = false;
            if (lastDurationMs && maxDurationMs && timer) {
                *lastDurationMs = timer->elapsed();
                *maxDurationMs = qMax(*maxDurationMs, *lastDurationMs);
            }
        }
    } guard{m_refreshingFaceStreamPathsPanel,
            &m_lastFaceStreamPanelRefreshDurationMs,
            &m_maxFaceStreamPanelRefreshDurationMs,
            &refreshTimer};

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
    m_faceStreamPanelRows = QJsonArray();
    if (m_widgets.speakerFaceStreamDetailsEdit) {
        m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
            QStringLiteral("Select a FaceStream path row to inspect full JSON."));
    }
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Select a detection row to inspect full JSON."));
    }
    if (m_widgets.speakerRawDetectionTable) {
        QSignalBlocker rawTableBlocker(m_widgets.speakerRawDetectionTable);
        m_widgets.speakerRawDetectionTable->clearContents();
        m_widgets.speakerRawDetectionTable->setRowCount(0);
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
    if (refreshSignature == m_faceStreamPanelRefreshSignature) {
        return;
    }

    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    QJsonObject continuityRoot;
    if (transcriptEngine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot)) {
        continuityRoot = continuityRootForClip(artifactRoot, clip->id);
    }
    if (m_widgets.speakerDetectionsAvailableCheckBox) {
        QSignalBlocker block(m_widgets.speakerDetectionsAvailableCheckBox);
        m_widgets.speakerDetectionsAvailableCheckBox->setChecked(
            !continuityRoot.value(QStringLiteral("raw_frames")).toArray().isEmpty());
    }
    if (m_widgets.speakerTracksAvailableCheckBox) {
        QSignalBlocker block(m_widgets.speakerTracksAvailableCheckBox);
        m_widgets.speakerTracksAvailableCheckBox->setChecked(
            !continuityRoot.value(QStringLiteral("raw_tracks")).toArray().isEmpty());
    }

    const QJsonArray streams = continuityStreamsForClip(*clip);
    m_faceStreamPanelRefreshSignature = refreshSignature;
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
        m_faceStreamPanelRows.push_back(row.streamObj);
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

    refreshRawDetectionsPanel(continuityRoot);
}

void SpeakersTab::refreshRawDetectionsPanel(const QJsonObject& continuityRoot)
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    auto finalizeRefreshTiming = [this, &refreshTimer]() {
        m_lastRawDetectionsPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxRawDetectionsPanelRefreshDurationMs =
            qMax(m_maxRawDetectionsPanelRefreshDurationMs, m_lastRawDetectionsPanelRefreshDurationMs);
    };
    if (!m_widgets.speakerRawDetectionTable) {
        finalizeRefreshTiming();
        return;
    }

    QSignalBlocker tableBlocker(m_widgets.speakerRawDetectionTable);
    m_widgets.speakerRawDetectionTable->clearContents();
    m_widgets.speakerRawDetectionTable->setRowCount(0);
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Select a detection row to inspect full JSON."));
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        finalizeRefreshTiming();
        return;
    }

    const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (rawFrames.isEmpty()) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("No raw detections were imported for this clip."));
        }
        finalizeRefreshTiming();
        return;
    }

    int64_t minFrame = std::numeric_limits<int64_t>::max();
    int64_t maxFrame = -1;
    for (const QJsonValue& frameValue : rawFrames) {
        const QJsonObject frameObj = frameValue.toObject();
        const int64_t frameNumber =
            frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (frameNumber < 0) {
            continue;
        }
        minFrame = qMin(minFrame, frameNumber);
        maxFrame = qMax(maxFrame, frameNumber);
    }

    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    if (!continuityPayloadFrameDomain(
            continuityRoot,
            QStringLiteral("raw_frames_frame_domain"),
            &frameDomain)) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("Raw detections are present, but frame-domain metadata is missing. "
                               "This artifact does not satisfy the current contract."));
        }
        finalizeRefreshTiming();
        return;
    }
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const QVector<RenderSyncMarker> markers =
        m_speakerDeps.getRenderSyncMarkers
            ? m_speakerDeps.getRenderSyncMarkers()
            : QVector<RenderSyncMarker>{};
    const int64_t absoluteSourceFrame =
        sourceFrameForClipAtTimelinePosition(*clip, static_cast<qreal>(timelineFrame), markers);
    const int64_t localTimelineFrame = qMax<int64_t>(0, timelineFrame - clip->startFrame);
    const int64_t localSourceFrame =
        qMax<int64_t>(0, absoluteSourceFrame - qMax<int64_t>(0, clip->sourceInFrame));
    const int64_t lookupFrame = facestreamLookupFrameForDomain(
        frameDomain, localTimelineFrame, localSourceFrame, absoluteSourceFrame);
    const QString frameLabel =
        frameDomain == FacestreamFrameDomain::ClipTimeline30Fps
            ? QStringLiteral("clip frame")
            : (frameDomain == FacestreamFrameDomain::SourceRelative
                   ? QStringLiteral("local source frame")
                   : QStringLiteral("source frame"));

    QJsonArray detectionsForFrame;
    for (const QJsonValue& frameValue : rawFrames) {
        const QJsonObject frameObj = frameValue.toObject();
        const int64_t frameNumber =
            frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (frameNumber != lookupFrame) {
            continue;
        }
        detectionsForFrame = frameObj.value(QStringLiteral("detections")).toArray();
        break;
    }

    if (detectionsForFrame.isEmpty()) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("No raw detections at %1 %2.")
                    .arg(frameLabel)
                    .arg(lookupFrame));
        }
        finalizeRefreshTiming();
        return;
    }

    m_widgets.speakerRawDetectionTable->setRowCount(detectionsForFrame.size());
    for (int row = 0; row < detectionsForFrame.size(); ++row) {
        const QJsonObject detectionObj = detectionsForFrame.at(row).toObject();
        auto* indexItem = new QTableWidgetItem(QString::number(row + 1));
        indexItem->setData(
            Qt::UserRole + 1,
            QString::fromUtf8(QJsonDocument(detectionObj).toJson(QJsonDocument::Indented)));
        auto* confItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("confidence")).toDouble(
                                detectionObj.value(QStringLiteral("score")).toDouble(0.0)),
                            'f',
                            3));
        auto* xItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("x")).toDouble(0.0), 'f', 3));
        auto* yItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("y")).toDouble(0.0), 'f', 3));
        auto* wItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("w")).toDouble(0.0), 'f', 3));
        auto* hItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("h")).toDouble(0.0), 'f', 3));
        m_widgets.speakerRawDetectionTable->setItem(row, 0, indexItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 1, confItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 2, xItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 3, yItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 4, wItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 5, hItem);
    }
    m_widgets.speakerRawDetectionTable->setCurrentCell(0, 0);
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Raw detections at %1 %2: %3\n\nSelect a row to inspect full JSON.")
                .arg(frameLabel)
                .arg(lookupFrame)
                .arg(detectionsForFrame.size()));
    }
    finalizeRefreshTiming();
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
                                         const QJsonArray& streams,
                                         const QString& speakerId,
                                         const QVector<int>& assignedTrackIds,
                                         editor::DecoderContext* decoderCtx,
                                         QHash<int64_t, QImage>* frameImageCache,
                                         qreal sourceFps)
{
    Q_UNUSED(transcriptRoot);
    const QJsonArray faceRefs = speakerFaceRefs(profile);
    for (int i = faceRefs.size() - 1; i >= 0; --i) {
        const QJsonObject previewKeyframe = previewKeyframeFromSpeakerFaceRef(faceRefs.at(i).toObject());
        if (!previewKeyframe.isEmpty()) {
            return faceStreamPreviewAvatarWithDecoder(
                clip, speakerId, previewKeyframe, 32, decoderCtx, frameImageCache, sourceFps);
        }
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
        return faceStreamPreviewAvatarWithDecoder(
            clip, speakerId, keyframes.first().toObject(), 32, decoderCtx, frameImageCache, sourceFps);
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
        return QStringLiteral("Speaker Tracking: Off");
    }
    const QString mode = tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
    const bool enabled = transcriptTrackingEnabled(tracking);
    const QString autoState = tracking.value(QString(kTranscriptSpeakerTrackingAutoStateKey)).toString();
    const int keyframeCount = tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();
    QString summary = QStringLiteral("Speaker Tracking: %1 (%2)")
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
