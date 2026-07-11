#include "speakers_tab.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "keyframe_table_shared.h"
#include "speakers_tab_internal.h"
#include "speakers_table.h"
#include "transcript_engine.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QListWidget>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QWheelEvent>
#include <QJsonDocument>

#include <limits>

void SpeakersTab::wire()
{
    if (!m_faceStreamPanelRefreshTimer) {
        m_faceStreamPanelRefreshTimer = new QTimer(this);
        m_faceStreamPanelRefreshTimer->setSingleShot(true);
        m_faceStreamPanelRefreshTimer->setInterval(200);
        connect(m_faceStreamPanelRefreshTimer, &QTimer::timeout, this, [this]() {
            m_faceStreamPanelRefreshQueued = false;
            refreshFaceDetectionsPathsPanel();
        });
    }
    if (!m_selectedSpeakerPanelRefreshTimer) {
        m_selectedSpeakerPanelRefreshTimer = new QTimer(this);
        m_selectedSpeakerPanelRefreshTimer->setSingleShot(true);
        m_selectedSpeakerPanelRefreshTimer->setInterval(80);
        connect(m_selectedSpeakerPanelRefreshTimer, &QTimer::timeout, this, [this]() {
            m_selectedSpeakerPanelRefreshQueued = false;
            updateSpeakerTrackingStatusLabel();
            updateSelectedSpeakerPanel();
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
    if (m_widgets.speakerExportLongSectionsButton) {
        m_widgets.speakerExportLongSectionsButton->setToolTip(
            QStringLiteral("Export contiguous transcript sections that meet the minimum word count."));
        connect(m_widgets.speakerExportLongSectionsButton,
                &QPushButton::clicked,
                this,
                &SpeakersTab::onSpeakerExportLongSectionsClicked);
    }
    if (m_widgets.speakerCreateTitleClipsButton) {
        connect(m_widgets.speakerCreateTitleClipsButton,
                &QPushButton::clicked,
                this,
                &SpeakersTab::onSpeakerCreateTitleClipsClicked);
    }
    if (m_widgets.speakerOverlayCreateTitleClipsButton) {
        connect(m_widgets.speakerOverlayCreateTitleClipsButton,
                &QCheckBox::toggled,
                this,
                [this](bool enabled) {
                    if (m_updating) return;
                    if (enabled) {
                        onSpeakerCreateTitleClipsClicked();
                        const TimelineClip* updatedClip = m_deps.getSelectedClip
                            ? m_deps.getSelectedClip() : nullptr;
                        if (!updatedClip || !updatedClip->speakerTitleEngineActive) {
                            QSignalBlocker blocker(m_widgets.speakerOverlayCreateTitleClipsButton);
                            m_widgets.speakerOverlayCreateTitleClipsButton->setChecked(false);
                        }
                        return;
                    }
                    const TimelineClip* clip = m_deps.getSelectedClip
                        ? m_deps.getSelectedClip() : nullptr;
                    if (!clip || !m_speakerDeps.updateClipById) return;
                    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
                    const QString clipId = clip->id;
                    m_speakerDeps.updateClipById(clipId, [](TimelineClip& editable) {
                        editable.titleKeyframes.clear();
                        editable.speakerTitleEngineActive = false;
                    });
                    if (m_speakerDeps.replaceSpeakerTitleClips) {
                        m_speakerDeps.replaceSpeakerTitleClips(clipId, {});
                    }
                    if (m_speakerDeps.refreshPreview) m_speakerDeps.refreshPreview();
                    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
                });

        auto refreshEnabledFlyIn = [this]() {
            const TimelineClip* selectedClip = m_deps.getSelectedClip
                ? m_deps.getSelectedClip() : nullptr;
            const bool hasGeneratedIntroductions =
                selectedClip && selectedClip->speakerTitleEngineActive;
            if (!m_updating && m_widgets.speakerOverlayCreateTitleClipsButton &&
                (m_widgets.speakerOverlayCreateTitleClipsButton->isChecked() ||
                 hasGeneratedIntroductions)) {
                onSpeakerCreateTitleClipsClicked();
            }
            if (!m_updating && m_deps.scheduleSaveState) {
                m_deps.scheduleSaveState();
            }
        };
        for (QDoubleSpinBox* spin : {
                 m_widgets.speakerOverlayFlyInDelaySpin,
                 m_widgets.speakerOverlayFlyInDurationSpin,
                 m_widgets.speakerOverlayFlyInTimeSpin,
                 m_widgets.speakerOverlayWrapRadiusSpin,
                 m_widgets.speakerOverlayWrapDepthSpin,
                 m_widgets.speakerOverlayWrapStartAngleSpin,
                 m_widgets.speakerOverlayWrapEndAngleSpin,
                 m_widgets.speakerOverlayWrapPitchSpin,
                 m_widgets.speakerOverlayWrapRollSpin,
                 m_widgets.speakerOverlayRotationXSpin,
                 m_widgets.speakerOverlayRotationYSpin,
                 m_widgets.speakerOverlayRotationZSpin,
                 m_widgets.speakerOverlayTitlePatternScaleSpin,
                 m_widgets.speakerOverlayTitleExtrudeDepthSpin,
                 m_widgets.speakerOverlayTitleBevelScaleSpin}) {
            if (spin) connect(spin, &QDoubleSpinBox::editingFinished,
                              this, refreshEnabledFlyIn);
        }
        for (QSpinBox* spin : {m_widgets.speakerOverlayTitleFontSizeSpin,
                               m_widgets.speakerOverlayTitleBoxWidthSpin}) {
            if (spin) connect(spin, &QSpinBox::editingFinished,
                              this, refreshEnabledFlyIn);
        }
        for (QComboBox* combo : {m_widgets.speakerOverlayFlyInStyleCombo,
                                 m_widgets.speakerOverlayTitleTextMaterialCombo,
                                 m_widgets.speakerOverlayTitleBorderMaterialCombo,
                                 m_widgets.speakerOverlayTitleExtrudeModeCombo}) {
            if (combo) connect(combo, qOverload<int>(&QComboBox::currentIndexChanged),
                               this, [refreshEnabledFlyIn](int) { refreshEnabledFlyIn(); });
        }
        for (QLineEdit* edit : {m_widgets.speakerOverlayTitleTextPatternPathEdit,
                                m_widgets.speakerOverlayTitleBorderPatternPathEdit}) {
            if (edit) connect(edit, &QLineEdit::editingFinished,
                              this, refreshEnabledFlyIn);
        }
        if (m_widgets.speakerOverlayTitleExtrudeCheckBox) {
            connect(m_widgets.speakerOverlayTitleExtrudeCheckBox, &QCheckBox::toggled,
                    this, [this, refreshEnabledFlyIn](bool enabled) {
                        if (m_widgets.speakerOverlayTitleExtrudeModeCombo)
                            m_widgets.speakerOverlayTitleExtrudeModeCombo->setEnabled(enabled);
                        if (m_widgets.speakerOverlayTitleExtrudeDepthSpin)
                            m_widgets.speakerOverlayTitleExtrudeDepthSpin->setEnabled(enabled);
                        if (m_widgets.speakerOverlayTitleBevelScaleSpin)
                            m_widgets.speakerOverlayTitleBevelScaleSpin->setEnabled(enabled);
                        refreshEnabledFlyIn();
                    });
            const bool enabled = m_widgets.speakerOverlayTitleExtrudeCheckBox->isChecked();
            if (m_widgets.speakerOverlayTitleExtrudeModeCombo)
                m_widgets.speakerOverlayTitleExtrudeModeCombo->setEnabled(enabled);
            if (m_widgets.speakerOverlayTitleExtrudeDepthSpin)
                m_widgets.speakerOverlayTitleExtrudeDepthSpin->setEnabled(enabled);
            if (m_widgets.speakerOverlayTitleBevelScaleSpin)
                m_widgets.speakerOverlayTitleBevelScaleSpin->setEnabled(enabled);
        }
        if (m_widgets.speakerOverlayTitleBackgroundCheckBox) {
            connect(m_widgets.speakerOverlayTitleBackgroundCheckBox, &QCheckBox::toggled,
                    this, [refreshEnabledFlyIn](bool) { refreshEnabledFlyIn(); });
        }
    }
    if (m_widgets.speakerSectionMinimumWordsSpin) {
        connect(m_widgets.speakerSectionMinimumWordsSpin,
                qOverload<int>(&QSpinBox::valueChanged),
                this,
                [this](int value) {
                    const int minimumWords = qBound(0, value, 1000);
                    const TimelineClip* selectedClip =
                        m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
                    if (selectedClip && m_speakerDeps.updateClipById) {
                        m_speakerDeps.updateClipById(selectedClip->id, [minimumWords](TimelineClip& clip) {
                            clip.speakerSectionMinimumWords = minimumWords;
                        });
                    }
                    m_speakerSectionsTableRefreshSignature.clear();
                    if (m_transcriptSession.hasObjectDocument() &&
                        m_widgets.speakerShowContiguousSectionsCheckBox &&
                        m_widgets.speakerShowContiguousSectionsCheckBox->isChecked()) {
                        refreshSpeakerSectionsTable(m_transcriptSession.rootObject());
                    }
                    if (m_speakerDeps.refreshPreview) {
                        m_speakerDeps.refreshPreview();
                    }
                    if (m_deps.scheduleSaveState) {
                        m_deps.scheduleSaveState();
                    }
                });
    }
    if (m_widgets.speakerHideUnidentifiedCheckBox) {
        connect(m_widgets.speakerHideUnidentifiedCheckBox, &QCheckBox::toggled, this, [this]() {
            m_speakersTableRefreshSignature.clear();
            m_speakerSectionsTableRefreshSignature.clear();
            if (m_transcriptSession.hasObjectDocument()) {
                syncSpeakerListMode();
                if (m_widgets.speakerShowContiguousSectionsCheckBox &&
                    m_widgets.speakerShowContiguousSectionsCheckBox->isChecked()) {
                    refreshSpeakerSectionsTable(m_transcriptSession.rootObject());
                } else {
                    refreshSpeakersTable(m_transcriptSession.rootObject(), selectedSpeakerId());
                }
            }
        });
    }
    if (m_widgets.speakerSectionsTable) {
        m_widgets.speakerSectionsTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_widgets.speakerSectionsTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        m_widgets.speakerSectionsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_widgets.speakerSectionsTable->setMinimumHeight(220);
        m_widgets.speakerSectionsTable->setEditTriggers(
            QAbstractItemView::DoubleClicked |
            QAbstractItemView::EditKeyPressed |
            QAbstractItemView::SelectedClicked);
        m_widgets.speakerSectionsTable->setMouseTracking(true);
        m_widgets.speakerSectionsTable->viewport()->setMouseTracking(true);
        m_widgets.speakerSectionsTable->viewport()->installEventFilter(this);
        m_widgets.speakerSectionsTable->setItemDelegate(
            new SpeakerSectionItemDelegate(m_widgets.speakerSectionsTable));
        connect(m_widgets.speakerSectionsTable,
                &QTableWidget::itemSelectionChanged,
                this,
                [this]() {
                    const int row =
                        m_widgets.speakerSectionsTable ? m_widgets.speakerSectionsTable->currentRow() : -1;
                    m_sectionSelectionTiming.begin(row);
                    if (row < 0) {
                        m_sectionSelectionTiming.finishSkipped(QStringLiteral("no_current_row"));
                        return;
                    }
                    QTableWidgetItem* speakerItem =
                        m_widgets.speakerSectionsTable->item(row, SpeakerSectionSpeakerColumn);
                    if (!speakerItem) {
                        m_sectionSelectionTiming.finishSkipped(QStringLiteral("missing_speaker_item"));
                        return;
                    }
                    if (speakerItem->data(SpeakerSectionRowTypeRole).toInt(0) != 0) {
                        m_sectionSelectionTiming.finishSkipped(QStringLiteral("keyframe_detail_row"));
                        return;
                    }
                    const QString speakerId = speakerItem->data(Qt::UserRole).toString().trimmed();
                    if (speakerId.isEmpty()) {
                        m_sectionSelectionTiming.finishSkipped(QStringLiteral("empty_speaker_id"));
                        return;
                    }
                    int64_t timelineFrame = -1;
                    m_sectionSelectionTiming.markStep(QStringLiteral("row_lookup_duration_ms"));
                    updateSpeakerFramingTargetControls();
                    selectSpeakerRowById(speakerId);
                    m_sectionSelectionTiming.markStep(QStringLiteral("select_speaker_row_duration_ms"));
                    m_lastSelectedSpeakerIdHint = speakerId;
                    focusSpeakerSectionTrackFromRow(row);
                    updateSpeakerTrackingStatusLabelFast();
                    m_sectionSelectionTiming.markStep(QStringLiteral("tracking_status_duration_ms"));
                    updateSelectedSpeakerPanelFast();
                    m_sectionSelectionTiming.markStep(QStringLiteral("fast_panel_duration_ms"));
                    if (seekToSpeakerSectionRow(row, &timelineFrame)) {
                        m_sectionSelectionTiming.setSectionContext(speakerId, timelineFrame);
                        m_sectionSelectionTiming.markStep(QStringLiteral("seek_duration_ms"));
                    } else {
                        m_sectionSelectionTiming.markStep(QStringLiteral("seek_skipped_duration_ms"));
                    }
                    scheduleSelectedSpeakerPanelRefresh();
                    m_sectionSelectionTiming.markStep(QStringLiteral("schedule_panel_refresh_duration_ms"));
                    m_sectionSelectionTiming.finish();
                });
        connect(m_widgets.speakerSectionsTable,
                &QTableWidget::itemClicked,
                this,
                [this](QTableWidgetItem* item) {
                    if (item) {
                        seekToSpeakerSectionRow(item->row());
                    }
                });
        connect(m_widgets.speakerSectionsTable,
                &QTableWidget::itemChanged,
                this,
                [this](QTableWidgetItem* item) {
                    if (!item || item->column() != SpeakerSectionRotationColumn) {
                        return;
                    }
                    int row = -1;
                    int column = -1;
                    if (!editor::findTableCellForItem(m_widgets.speakerSectionsTable, item, &row, &column) ||
                        column != SpeakerSectionRotationColumn ||
                        m_updatingSpeakerFramingTargetControls) {
                        return;
                    }
                    QTableWidgetItem* liveItem = m_widgets.speakerSectionsTable->item(row, column);
                    if (!liveItem) {
                        return;
                    }
                    bool ok = false;
                    const qreal rotation = liveItem->text().trimmed().toDouble(&ok);
                    if (!ok) {
                        QSignalBlocker blocker(m_widgets.speakerSectionsTable);
                        const qreal previous =
                            qBound<qreal>(-180.0,
                                          liveItem->data(SpeakerSectionRotationRole).toDouble(),
                                          180.0);
                        liveItem->setText(QString::number(previous, 'f', 1));
                        return;
                    }
                    const qreal boundedRotation = qBound<qreal>(-180.0, rotation, 180.0);
                    if (!saveSpeakerSectionRotation(item->row(), boundedRotation)) {
                        QSignalBlocker blocker(m_widgets.speakerSectionsTable);
                        const qreal previous =
                            qBound<qreal>(-180.0,
                                          liveItem->data(SpeakerSectionRotationRole).toDouble(),
                                          180.0);
                        liveItem->setText(QString::number(previous, 'f', 1));
                        return;
                    }
                    if (m_widgets.speakerSectionRotationSpin &&
                        m_widgets.speakerSectionsTable->currentRow() == row) {
                        QSignalBlocker blocker(m_widgets.speakerSectionRotationSpin);
                        m_widgets.speakerSectionRotationSpin->setValue(boundedRotation);
                    }
                    if (m_speakerDeps.refreshPreview) {
                        m_speakerDeps.refreshPreview();
                    }
                    updateSpeakerTrackingStatusLabel();
                });
        connect(m_widgets.speakerSectionsTable,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::onSpeakerSectionsTableContextMenuRequested);
        connect(m_widgets.speakerSectionsTable,
                &QTableWidget::cellClicked,
                this,
                [this](int row, int column) {
                    if (column != SpeakerSectionIndexColumn || row < 0) {
                        return;
                    }
                    QTableWidgetItem* item =
                        m_widgets.speakerSectionsTable->item(row, SpeakerSectionIndexColumn);
                    if (!item || item->data(SpeakerSectionRowTypeRole).toInt(0) != 0) {
                        return;
                    }
                    const QString sectionKey = item->data(SpeakerSectionKeyRole).toString().trimmed();
                    if (sectionKey.isEmpty()) {
                        return;
                    }
                    if (m_expandedSpeakerSectionKeys.contains(sectionKey)) {
                        m_expandedSpeakerSectionKeys.remove(sectionKey);
                    } else {
                        m_expandedSpeakerSectionKeys.insert(sectionKey);
                    }
                    m_speakerSectionsTableRefreshSignature.clear();
                    if (m_transcriptSession.hasObjectDocument()) {
                        refreshSpeakerSectionsTable(m_transcriptSession.rootObject());
                    }
                });
    }
    if (m_widgets.speakerFaceDetectionsTable) {
        m_widgets.speakerFaceDetectionsTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.speakerFaceDetectionsTable,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::onSpeakerFaceDetectionsTableContextMenuRequested);
        connect(m_widgets.speakerFaceDetectionsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
            if (!m_widgets.speakerFaceDetectionsTable || !m_widgets.speakerFaceDetectionsDetailsEdit) {
                return;
            }
            const int row = m_widgets.speakerFaceDetectionsTable->currentRow();
            if (row < 0) {
                m_widgets.speakerFaceDetectionsDetailsEdit->setPlainText(
                    QStringLiteral("Select a FaceDetections path row to inspect full JSON."));
                return;
            }
            QTableWidgetItem* streamItem = m_widgets.speakerFaceDetectionsTable->item(row, 0);
            const int64_t storedFrame = streamItem
                ? streamItem->data(Qt::UserRole + 2).toLongLong()
                : -1;
            if (storedFrame >= 0 && m_deps.seekToTimelineFrame) {
                const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
                if (clip) {
                    QJsonObject streamObj;
                    const int rowIndex = streamItem ? streamItem->data(Qt::UserRole + 1).toInt() : -1;
                    if (rowIndex >= 0 && rowIndex < m_faceStreamPanelRows.size()) {
                        streamObj = m_faceStreamPanelRows.at(rowIndex).toObject();
                    }
                    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
                    if (!streamObj.isEmpty() &&
                        !parseFacestreamFrameDomainString(
                            streamObj.value(QStringLiteral("frame_domain")).toString(),
                            &frameDomain)) {
                        int64_t minFrame = std::numeric_limits<int64_t>::max();
                        int64_t maxFrame = std::numeric_limits<int64_t>::min();
                        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
                        for (const QJsonValue& keyframeValue : keyframes) {
                            const int64_t frame = keyframeValue
                                .toObject()
                                .value(QString(kTranscriptSpeakerTrackingFrameKey))
                                .toVariant()
                                .toLongLong();
                            minFrame = qMin(minFrame, frame);
                            maxFrame = qMax(maxFrame, frame);
                        }
                        if (minFrame <= maxFrame) {
                            frameDomain = inferFacestreamFrameDomain(*clip, minFrame, maxFrame);
                        }
                    }

                    int64_t timelineFrame = clip->startFrame + storedFrame;
                    if (frameDomain != FacestreamFrameDomain::ClipTimeline30Fps) {
                        const QVector<RenderSyncMarker> renderSyncMarkers =
                            m_speakerDeps.getRenderSyncMarkers
                                ? m_speakerDeps.getRenderSyncMarkers()
                                : QVector<RenderSyncMarker>{};
                        const int64_t sourceFrame =
                            mapFacestreamFrameToSourceFrame(*clip, storedFrame, frameDomain, renderSyncMarkers);
                        const qreal sourceFps = resolvedSourceFps(*clip);
                        const qreal sourceOffset = qMax<qreal>(
                            0.0,
                            static_cast<qreal>(sourceFrame - clip->sourceInFrame));
                        const qreal rate = qMax<qreal>(0.001, clip->playbackRate);
                        const int64_t localTimelineFrame = qMax<int64_t>(
                            0,
                            static_cast<int64_t>(std::floor(
                                (sourceOffset / (qMax<qreal>(0.001, sourceFps) * rate)) *
                                static_cast<qreal>(kTimelineFps))));
                        timelineFrame = clip->startFrame + localTimelineFrame;
                    }
                    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
                    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
                    m_deps.seekToTimelineFrame(timelineFrame);
                }
            }
            QString streamJson;
            if (streamItem) {
                const int rowIndex = streamItem->data(Qt::UserRole + 1).toInt();
                if (rowIndex >= 0 && rowIndex < m_faceStreamPanelRows.size()) {
                    QJsonObject streamObj = m_faceStreamPanelRows.at(rowIndex).toObject();
                    if (!streamObj.contains(QStringLiteral("keyframes"))) {
                        const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
                        if (clip) {
                            editor::TranscriptEngine transcriptEngine;
                            QJsonObject artifactRoot;
                            if (transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot) ||
                                transcriptEngine.loadFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
                                const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip->id);
                                const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
                                const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
                                const QJsonArray loadedStreams =
                                    jcut::facedetections::continuityStreamsForAssignments(
                                        continuityRoot,
                                        trackId >= 0 ? QSet<int>{trackId} : QSet<int>{},
                                        streamId.isEmpty() ? QSet<QString>{} : QSet<QString>{streamId},
                                        m_transcriptSession.rootObject());
                                if (!loadedStreams.isEmpty()) {
                                    streamObj = loadedStreams.first().toObject();
                                }
                            }
                        }
                    }
                    streamJson = QString::fromUtf8(QJsonDocument(streamObj).toJson(QJsonDocument::Indented));
                }
            }
            m_widgets.speakerFaceDetectionsDetailsEdit->setPlainText(
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
                this, &SpeakersTab::onSpeakerViewFaceDetectionsClicked);
    }
    if (m_widgets.speakerFacestreamSettingsButton) {
        m_widgets.speakerFacestreamSettingsButton->setToolTip(
            QStringLiteral("Open continuity-track rebuild and smoothing tools."));
        connect(m_widgets.speakerFacestreamSettingsButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerFaceDetectionsSettingsClicked);
    }
    if (m_widgets.speakerRefreshTrackAvatarsButton) {
        m_widgets.speakerRefreshTrackAvatarsButton->setToolTip(
            QStringLiteral("Regenerate and persist representative avatar crops for continuity tracks."));
        connect(m_widgets.speakerRefreshTrackAvatarsButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRefreshTrackAvatarsClicked);
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
    if (m_widgets.speakerPlayheadFaceDetectionsList) {
        m_widgets.speakerPlayheadFaceDetectionsList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.speakerPlayheadFaceDetectionsList,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::clearPlayheadTrackAssignmentAt);
        connect(m_widgets.speakerPlayheadFaceDetectionsList, &QListWidget::itemSelectionChanged, this, [this]() {
            const bool hasSpeaker = !selectedSpeakerId().trimmed().isEmpty();
            const bool hasSelection =
                m_widgets.speakerPlayheadFaceDetectionsList &&
                !m_widgets.speakerPlayheadFaceDetectionsList->selectedItems().isEmpty();
            if (m_widgets.speakerPrecropFacesButton) {
                const bool playheadListVisible =
                    !m_widgets.speakerShowPlayheadFaceDetectionsCheckBox ||
                    m_widgets.speakerShowPlayheadFaceDetectionsCheckBox->isChecked();
                m_widgets.speakerPrecropFacesButton->setEnabled(
                    activeCutMutable() && playheadListVisible && hasSpeaker && hasSelection);
            }
        });
    }
    if (m_widgets.speakerShowPlayheadFaceDetectionsCheckBox) {
        connect(m_widgets.speakerShowPlayheadFaceDetectionsCheckBox, &QCheckBox::toggled, this, [this]() {
            updatePlayheadTrackCandidatesVisibility();
        });
    }
    if (m_widgets.selectedSpeakerFaceDetectionsList) {
        m_widgets.selectedSpeakerFaceDetectionsList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.selectedSpeakerFaceDetectionsList,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::showSelectedSpeakerAssignedTracksContextMenu);
        auto* deassignShortcut = new QAction(m_widgets.selectedSpeakerFaceDetectionsList);
        deassignShortcut->setShortcut(QKeySequence::Delete);
        deassignShortcut->setShortcutContext(Qt::WidgetShortcut);
        connect(deassignShortcut, &QAction::triggered, this, [this]() {
            deassignSelectedSpeakerAssignedTracks();
        });
        m_widgets.selectedSpeakerFaceDetectionsList->addAction(deassignShortcut);
    }
    if (m_widgets.selectedSpeakerNameEdit) {
        m_widgets.selectedSpeakerNameEdit->setToolTip(
            QStringLiteral("Edit the selected speaker's display name."));
        connect(m_widgets.selectedSpeakerNameEdit, &QLineEdit::editingFinished, this, [this]() {
            if (m_updating || !activeCutMutable() || !m_widgets.selectedSpeakerNameEdit) {
                return;
            }
            if (!saveSelectedSpeakerProfileField(QString(kTranscriptSpeakerNameKey),
                                                 m_widgets.selectedSpeakerNameEdit->text())) {
                updateSelectedSpeakerPanel();
            }
        });
    }
    if (m_widgets.selectedSpeakerOrganizationEdit) {
        m_widgets.selectedSpeakerOrganizationEdit->setToolTip(
            QStringLiteral("Edit the selected speaker's organization."));
        connect(m_widgets.selectedSpeakerOrganizationEdit, &QLineEdit::editingFinished, this, [this]() {
            if (m_updating || !activeCutMutable() || !m_widgets.selectedSpeakerOrganizationEdit) {
                return;
            }
            if (!saveSelectedSpeakerProfileField(QStringLiteral("organization"),
                                                 m_widgets.selectedSpeakerOrganizationEdit->text())) {
                updateSelectedSpeakerPanel();
            }
        });
    }
    auto connectSelectedSpeakerProfileEdit =
        [this](QLineEdit* edit, const QString& fieldKey, const QString& tooltip, bool validateColor) {
            if (!edit) {
                return;
            }
            edit->setToolTip(tooltip);
            connect(edit, &QLineEdit::editingFinished, this, [this, edit, fieldKey, validateColor]() {
                if (m_updating || !activeCutMutable() || !edit) {
                    return;
                }
                QString value = edit->text().trimmed();
                if (validateColor && !value.isEmpty()) {
                    const QColor color(value);
                    if (!color.isValid()) {
                        updateSelectedSpeakerPanel();
                        return;
                    }
                    value = color.name(QColor::HexRgb);
                }
                if (!saveSelectedSpeakerProfileField(fieldKey, value)) {
                    updateSelectedSpeakerPanel();
                }
            });
        };
    connectSelectedSpeakerProfileEdit(m_widgets.selectedSpeakerLogoPathEdit,
                                      QString(kTranscriptSpeakerLogoPathKey),
                                      QStringLiteral("Logo image path used inside generated speaking-time titles."),
                                      false);
    connectSelectedSpeakerProfileEdit(m_widgets.selectedSpeakerPrimaryColorEdit,
                                      QString(kTranscriptSpeakerPrimaryColorKey),
                                      QStringLiteral("Primary title color, usually used for speaker title text."),
                                      true);
    connectSelectedSpeakerProfileEdit(m_widgets.selectedSpeakerSecondaryColorEdit,
                                      QString(kTranscriptSpeakerSecondaryColorKey),
                                      QStringLiteral("Secondary title color, usually used for the title backing plate."),
                                      true);
    connectSelectedSpeakerProfileEdit(m_widgets.selectedSpeakerAccentColorEdit,
                                      QString(kTranscriptSpeakerAccentColorKey),
                                      QStringLiteral("Accent title color, usually used for borders and highlights."),
                                      true);
    if (m_widgets.speakerAiFindNamesButton) {
        m_widgets.speakerAiFindNamesButton->setToolTip(
            QStringLiteral("Mine transcript text for speaker profile names only; organizations stay in the Organization field."));
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
    if (m_widgets.speakerSectionRotationSpin) {
        connect(m_widgets.speakerSectionRotationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        connect(m_widgets.speakerFramingZoomEnabledCheckBox, &QCheckBox::toggled,
                this, &SpeakersTab::onSpeakerFramingZoomEnabledChanged);
    }
    if (m_widgets.speakerFramingCenterSmoothingFramesSpin) {
        connect(m_widgets.speakerFramingCenterSmoothingFramesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int) { onSpeakerFramingTargetChanged(); });
    }
    if (m_widgets.speakerFramingZoomSmoothingFramesSpin) {
        connect(m_widgets.speakerFramingZoomSmoothingFramesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int) { onSpeakerFramingTargetChanged(); });
    }
    if (m_widgets.speakerFramingSmoothingModeCombo) {
        connect(m_widgets.speakerFramingSmoothingModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { onSpeakerFramingTargetChanged(); });
    }
    if (m_widgets.speakerFramingCenterSmoothingStrengthSpin) {
        connect(m_widgets.speakerFramingCenterSmoothingStrengthSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this](double) { onSpeakerFramingTargetChanged(); });
    }
    if (m_widgets.speakerFramingZoomSmoothingStrengthSpin) {
        connect(m_widgets.speakerFramingZoomSmoothingStrengthSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this](double) { onSpeakerFramingTargetChanged(); });
    }
    if (m_widgets.speakerFramingGapHoldFramesSpin) {
        connect(m_widgets.speakerFramingGapHoldFramesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int) { onSpeakerFramingTargetChanged(); });
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        connect(m_widgets.speakerApplyFramingToClipCheckBox, &QCheckBox::toggled,
                this, &SpeakersTab::onSpeakerApplyFramingToClipChanged);
    }
    if (m_widgets.speakerFramingEnabledKeyframeTable) {
        m_widgets.speakerFramingEnabledKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.speakerFramingEnabledKeyframeTable,
                &QTableWidget::itemSelectionChanged,
                this,
                &SpeakersTab::onSpeakerFramingEnabledTableSelectionChanged);
        connect(m_widgets.speakerFramingEnabledKeyframeTable,
                &QTableWidget::itemChanged,
                this,
                &SpeakersTab::onSpeakerFramingEnabledTableItemChanged);
        connect(m_widgets.speakerFramingEnabledKeyframeTable,
                &QWidget::customContextMenuRequested,
                this,
                &SpeakersTab::onSpeakerFramingEnabledTableContextMenu);
        m_widgets.speakerFramingEnabledKeyframeTable->installEventFilter(this);
        m_widgets.speakerFramingEnabledKeyframeTable->viewport()->installEventFilter(this);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        connect(m_widgets.selectedSpeakerPreviousSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPreviousSentenceClicked);
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        connect(m_widgets.selectedSpeakerNextSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerNextSentenceClicked);
    }
    if (m_widgets.selectedSpeakerNextSectionButton) {
        connect(m_widgets.selectedSpeakerNextSectionButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerNextSectionClicked);
    }
    if (m_widgets.selectedSpeakerRandomSentenceButton) {
        connect(m_widgets.selectedSpeakerRandomSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRandomSentenceClicked);
    }
    updatePlayheadTrackCandidatesVisibility();
    syncSpeakerListMode();
}

bool SpeakersTab::eventFilter(QObject* watched, QEvent* event)
{
    if (event &&
        m_widgets.speakerSectionsTable &&
        watched == m_widgets.speakerSectionsTable->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QTableWidgetItem* item = m_widgets.speakerSectionsTable->itemAt(mouseEvent->pos());
            if (item && item->data(SpeakerSectionRowTypeRole).toInt(0) == 1) {
                showSpeakerSectionKeyframeHoverPreview(
                    item->row(),
                    m_widgets.speakerSectionsTable->viewport()->mapToGlobal(mouseEvent->pos()));
            } else {
                hideSpeakerSectionKeyframeHoverPreview();
            }
            break;
        }
        case QEvent::ToolTip: {
            auto* helpEvent = static_cast<QHelpEvent*>(event);
            const QTableWidgetItem* item = m_widgets.speakerSectionsTable->itemAt(helpEvent->pos());
            if (item && item->data(SpeakerSectionRowTypeRole).toInt(0) == 1) {
                showSpeakerSectionKeyframeHoverPreview(item->row(), helpEvent->globalPos());
                return true;
            }
            break;
        }
        case QEvent::Leave:
        case QEvent::Hide:
            hideSpeakerSectionKeyframeHoverPreview();
            break;
        default:
            break;
        }
    }

    if (event && event->type() == QEvent::KeyPress &&
        m_widgets.speakerFramingEnabledKeyframeTable &&
        (watched == m_widgets.speakerFramingEnabledKeyframeTable ||
         watched == m_widgets.speakerFramingEnabledKeyframeTable->viewport())) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace) {
            return removeSelectedSpeakerFramingEnabledKeyframes();
        }
    }
    return QObject::eventFilter(watched, event);
}
