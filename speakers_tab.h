#pragma once

#include <QJsonDocument>
#include <QJsonArray>
#include <QFutureWatcher>
#include <QPixmap>
#include <QTableWidget>
#include <QHash>
#include <QPoint>
#include <QStringList>
#include <functional>

#include "editor_action_result.h"
#include "editor_playback_types.h"
#include "editor_timeline_types.h"
#include "table_tab_base.h"
#include "transcript_document_io.h"
#include "transcript_document_session.h"

class QLabel;
class QListWidget;
class QPushButton;
class QDoubleSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QTimer;
class QToolButton;
class SpeakersTable;
namespace editor {
class DecoderContext;
}

class SpeakersTab : public TableTabBase {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* speakersInspectorClipLabel = nullptr;
        QLabel* speakersInspectorDetailsLabel = nullptr;
        QTableWidget* speakersTable = nullptr;
        QCheckBox* speakerShowContiguousSectionsCheckBox = nullptr;
        QTableWidget* speakerSectionsTable = nullptr;
        QLabel* selectedSpeakerIdLabel = nullptr;
        QListWidget* selectedSpeakerFaceStreamsList = nullptr;
        QLabel* selectedSpeakerRef1ImageLabel = nullptr;
        QLabel* selectedSpeakerRef2ImageLabel = nullptr;
        QPushButton* selectedSpeakerPreviousSentenceButton = nullptr;
        QPushButton* selectedSpeakerNextSentenceButton = nullptr;
        QPushButton* selectedSpeakerRandomSentenceButton = nullptr;
        QLabel* speakerCurrentSentenceLabel = nullptr;
        QPushButton* speakerSetReference1Button = nullptr;
        QPushButton* speakerSetReference2Button = nullptr;
        QPushButton* speakerPickReference1Button = nullptr;
        QPushButton* speakerPickReference2Button = nullptr;
        QPushButton* speakerClearReferencesButton = nullptr;
        QPushButton* speakerRunAutoTrackButton = nullptr;
        QPushButton* speakerViewFacestreamButton = nullptr;
        QPushButton* speakerFacestreamSettingsButton = nullptr;
        QPushButton* speakerEnableTrackingButton = nullptr;
        QPushButton* speakerDisableTrackingButton = nullptr;
        QPushButton* speakerDeletePointstreamButton = nullptr;
        QPushButton* speakerGuideButton = nullptr;
        QPushButton* speakerPrecropFacesButton = nullptr;
        QPushButton* speakerAiFindNamesButton = nullptr;
        QPushButton* speakerAiFindOrganizationsButton = nullptr;
        QPushButton* speakerAiCleanAssignmentsButton = nullptr;
        QLabel* speakerTrackingStatusLabel = nullptr;
        QDoubleSpinBox* speakerFramingTargetXSpin = nullptr;
        QDoubleSpinBox* speakerFramingTargetYSpin = nullptr;
        QDoubleSpinBox* speakerFramingTargetBoxSpin = nullptr;
        QCheckBox* speakerFramingZoomEnabledCheckBox = nullptr;
        QCheckBox* speakerApplyFramingToClipCheckBox = nullptr;
        QLabel* speakerClipFramingStatusLabel = nullptr;
        QLabel* speakerRefsChipLabel = nullptr;
        QLabel* speakerPointstreamChipLabel = nullptr;
        QPushButton* speakerTrackingChipButton = nullptr;
        QPushButton* speakerStabilizeChipButton = nullptr;
        QTableWidget* speakerFaceStreamTable = nullptr;
        QPlainTextEdit* speakerFaceStreamDetailsEdit = nullptr;
        QCheckBox* speakerDetectionsAvailableCheckBox = nullptr;
        QCheckBox* speakerTracksAvailableCheckBox = nullptr;
        QTableWidget* speakerRawDetectionTable = nullptr;
        QPlainTextEdit* speakerRawDetectionDetailsEdit = nullptr;
    };

    struct Dependencies : public TableTabBase::Dependencies {
        std::function<QVector<RenderSyncMarker>()> getRenderSyncMarkers;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<QSize()> getOutputSize;
        std::function<void()> refreshPreview;
        std::function<void(const QString&)> setPreviewMode;
        std::function<bool(QString*)> ensureAiSession;
        std::function<void(const QStringList&)> exportSpeakersVideo;
    };

    explicit SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~SpeakersTab() override = default;

    void wire();
    void refresh();
    void refreshForSubtab(const QString& subtabName);
    void syncCurrentSpeakerSentenceToPlayhead();
    bool generateFaceStreamForSelectedClip();
    bool deleteFaceStreamForSelectedClip(bool confirmDialog = true,
                                         QString* errorOut = nullptr);
    editor::ActionResult deleteFaceStreamForSelectedClipResult(bool confirmDialog = true,
                                                               bool interactive = true);
    bool handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm);
    bool handlePreviewBox(const QString& clipId, qreal xNorm, qreal yNorm, qreal boxSizeNorm);
    bool handlePreviewFaceStreamBox(const QString& clipId,
                                    int trackId,
                                    const QString& streamId,
                                    int64_t sourceFrame,
                                    qreal xNorm,
                                    qreal yNorm,
                                    qreal boxSizeNorm);
    bool runAiFindSpeakerNames();
    bool runAiFindOrganizations();
    bool runAiCleanSpuriousAssignments();
    bool rebuildProcessedFaceStreamForSelectedClip(bool interactive = true);
    qint64 lastSpeakersTableRefreshDurationMs() const { return m_lastSpeakersTableRefreshDurationMs; }
    qint64 maxSpeakersTableRefreshDurationMs() const { return m_maxSpeakersTableRefreshDurationMs; }
    qint64 lastFaceStreamPanelRefreshDurationMs() const { return m_lastFaceStreamPanelRefreshDurationMs; }
    qint64 maxFaceStreamPanelRefreshDurationMs() const { return m_maxFaceStreamPanelRefreshDurationMs; }
    qint64 lastRawDetectionsPanelRefreshDurationMs() const { return m_lastRawDetectionsPanelRefreshDurationMs; }
    qint64 maxRawDetectionsPanelRefreshDurationMs() const { return m_maxRawDetectionsPanelRefreshDurationMs; }

signals:
    void transcriptDocumentChanged();

private slots:
    void onSpeakersTableItemChanged(QTableWidgetItem* item);
    void onSpeakersTableItemClicked(QTableWidgetItem* item);
    void onSpeakersSelectionChanged();
    void onSpeakersTableContextMenuRequested(const QPoint& pos);
    void onSpeakerSetReference1Clicked();
    void onSpeakerSetReference2Clicked();
    void onSpeakerPickReference1Clicked();
    void onSpeakerPickReference2Clicked();
    void onSpeakerPreviousSentenceClicked();
    void onSpeakerNextSentenceClicked();
    void onSpeakerRandomSentenceClicked();
    void onSpeakerClearReferencesClicked();
    void onSpeakerRunAutoTrackClicked();
    void onSpeakerViewFaceStreamClicked();
    void onSpeakerFaceStreamSettingsClicked();
    void onSpeakerEnableTrackingClicked();
    void onSpeakerDisableTrackingClicked();
    void onSpeakerDeletePointstreamClicked();
    void onSpeakerTrackingChipClicked();
    void onSpeakerStabilizeChipClicked();
    void onSpeakerGuideClicked();
    void onSpeakerPrecropFacesClicked();
    void onSpeakerFramingTargetChanged();
    void onSpeakerFramingZoomEnabledChanged(bool checked);
    void onSpeakerApplyFramingToClipChanged(bool checked);
    void onSpeakerFaceStreamTableContextMenuRequested(const QPoint& pos);
    void onSpeakerFindMatchingTracksClicked();

private:
    bool updateLoadedTranscriptDocument(const std::function<bool(QJsonObject&)>& mutator);
    bool saveLoadedTranscriptDocument();
    void queueLoadedTranscriptDocumentSave();
    void startTranscriptLoadRequest(const QString& clipFilePath,
                                    const QString& transcriptPath,
                                    const QString& preferredSpeakerId);
    void applyLoadedTranscriptDocumentData(const TimelineClip& clip, const QString& preferredSpeakerId);
    void requestRefreshFaceStreamPathsPanel();
    enum class SentenceNavAction {
        Previous,
        Next,
        Random
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    void refreshFaceStreamPathsPanel();
    void refreshSpeakerSectionsTable(const QJsonObject& transcriptRoot);
    void syncSpeakerListMode();
    bool selectSpeakerRowById(const QString& speakerId);
    void refreshRawDetectionsPanel(const QJsonObject& continuityRoot);
    bool openReferencePreviewWindow(int referenceIndex);
    QPixmap referenceFullFramePreview(const TimelineClip& clip,
                                      const QString& speakerId,
                                      const QJsonObject& refObj,
                                      QSize targetSize = QSize(960, 540));
    QPixmap faceStreamPreviewAvatar(const TimelineClip& clip,
                                    const QString& speakerId,
                                    const QJsonObject& keyframeObj,
                                    int size = 72) const;
    QPixmap faceStreamPreviewAvatarWithDecoder(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& keyframeObj,
                                               int size,
                                               editor::DecoderContext* decoderCtx,
                                               QHash<int64_t, QImage>* frameImageCache,
                                               qreal sourceFps) const;
    QVector<QPixmap> assignedFaceStreamPreviewPixmaps(const TimelineClip& clip,
                                                      const QString& speakerId) const;
    QString assignedFaceStreamPreviewTooltipHtml(const TimelineClip& clip,
                                                 const QString& speakerId) const;
    QJsonArray continuityStreamsForClip(const TimelineClip& clip) const;
    void clearFaceStreamDerivedCaches();
    QJsonObject resolveFaceStreamAssignmentRow(const TimelineClip& clip,
                                               const QJsonArray& streams,
                                               const QJsonObject& row) const;
    QHash<int, QString> resolvedIdentityByTrackId(const TimelineClip& clip,
                                                  const QJsonArray& streams) const;
    QVector<int> resolvedAssignedTrackIdsForSpeaker(const TimelineClip& clip,
                                                    const QJsonArray& streams,
                                                    const QString& speakerId) const;
    void showSpeakerAvatarHoverPreview(const QString& speakerId, const QPoint& globalPos);
    void hideSpeakerAvatarHoverPreview();
    bool selectedClipHasFaceStreamSidecars() const;
    bool clipSupportsTranscript(const TimelineClip& clip) const;
    bool activeCutMutable() const;
    QString originalTranscriptPathForClip(const QString& clipFilePath) const;
    QString selectedSpeakerId() const;
    QString speakerDisplayName(const QString& speakerId) const;
    QString speakerDisplayLabel(const QString& speakerId) const;
    QString speakerTrackingSummary(const QJsonObject& profile) const;
    bool ensureAiActionReady(const QString& actionTitle) const;
    void updateSpeakerTrackingStatusLabel();
    void updateSelectedSpeakerPanel();
    QString currentSpeakerSentenceAtCurrentFrame(const QString& speakerId) const;
    int64_t currentSourceFrameForClip(const TimelineClip& clip) const;
    void refreshSpeakersTable(const QJsonObject& transcriptRoot,
                              const QString& preferredSpeakerId = QString());
    void seekToSpeakerFirstWord(const QString& speakerId);
    bool seekToSpeakerSegmentRelative(const QString& speakerId, int direction);
    bool seekToSpeakerRandomSentence(const QString& speakerId);
    bool cycleFramingModeForSpeaker(const QString& speakerId);
    bool navigateSpeakerSentence(const QString& speakerId, SentenceNavAction action);
    QVector<int64_t> speakerSourceFrames(const QJsonObject& transcriptRoot,
                                         const QString& speakerId) const;
    int64_t firstSourceFrameForSpeaker(const QJsonObject& transcriptRoot, const QString& speakerId) const;
    QPixmap speakerAvatarForRow(const TimelineClip& clip,
                                const QJsonObject& transcriptRoot,
                                const QJsonObject& profile,
                                const QJsonArray& streams,
                                const QString& speakerId,
                                const QVector<int>& assignedTrackIds = {},
                                editor::DecoderContext* decoderCtx = nullptr,
                                QHash<int64_t, QImage>* frameImageCache = nullptr,
                                qreal sourceFps = 0.0);
    QPixmap speakerReferenceAvatar(const TimelineClip& clip,
                                   const QString& speakerId,
                                   const QJsonObject& refObj,
                                   int size = 28);
    QPixmap unsetSpeakerAvatar(int size) const;
    QPixmap placeholderSpeakerAvatar(const QString& speakerId) const;
    bool saveSpeakerProfileEdit(int tableRow, int column, const QString& valueText);
    bool saveSpeakerTrackingReferenceAt(const QString& speakerId,
                                        int referenceIndex,
                                        int64_t frame,
                                        qreal xNorm,
                                        qreal yNorm,
                                        qreal boxSizeNorm = -1.0);
    bool saveSpeakerTrackingReference(const QString& speakerId, int referenceIndex);
    bool assignTrackToSpeaker(const QString& speakerId,
                              int trackId,
                              const QString& streamId,
                              int64_t sourceFrame,
                              qreal xNorm,
                              qreal yNorm,
                              qreal boxSizeNorm,
                              const QString& resolutionSource);
    bool assignTrackAnchorsToSpeakerBatch(const QString& speakerId,
                                          const QJsonArray& trackAnchors,
                                          const QString& resolutionSource,
                                          const QString& auditAction);
    void openTrackPickerForSpeaker(const QString& speakerId);
    bool armReferencePickForSpeaker(const QString& speakerId, int referenceIndex);
    bool clearSpeakerTrackingReferences(const QString& speakerId);
    bool deleteSpeakerAutoTrackPointstream(const QString& speakerId);
    bool setSpeakerTrackingEnabled(const QString& speakerId, bool enabled);
    bool setSpeakerSkipped(const QString& speakerId, bool skipped);
    bool beginSelectedReferenceAvatarDrag(int referenceIndex, const QPoint& localPos);
    void updateSelectedReferenceAvatarDrag(const QPoint& localPos);
    void finishSelectedReferenceAvatarDrag(bool commit);
    bool selectedSpeakerReferenceObject(int referenceIndex,
                                        QString* speakerIdOut,
                                        QJsonObject* refOut) const;
    bool saveClipSpeakerFramingTargetsFromControls();
    bool saveClipSpeakerFramingEnabledFromControls();
    void updateSpeakerFramingTargetControls();
    bool adjustSelectedReferenceAvatarZoom(int referenceIndex, int wheelDelta);
    QPointF referenceNormPerPixelFromSourceFrame(const TimelineClip& clip,
                                                 const QJsonObject& refObj,
                                                 int avatarSize) const;

    Widgets m_widgets;
    Dependencies m_speakerDeps;
    TranscriptDocumentSession m_transcriptSession{QStringLiteral("speakers")};
    mutable QHash<QString, QPixmap> m_avatarCache;
    mutable QHash<QString, QJsonArray> m_continuityStreamsCache;
    int m_pendingReferencePick = 0;
    bool m_selectedAvatarDragActive = false;
    int m_selectedAvatarDragReferenceIndex = 0;
    QString m_selectedAvatarDragSpeakerId;
    QPoint m_selectedAvatarDragLastPos;
    QJsonObject m_selectedAvatarDragRefObj;
    QPointF m_selectedAvatarDragNormPerPixel;
    QString m_lastSelectedSpeakerIdHint;
    bool m_updatingSpeakerFramingTargetControls = false;
    QString m_lastSelectionSeekSpeakerId;
    QString m_lastSelectionSeekClipId;
    bool m_refreshingFaceStreamPathsPanel = false;
    bool m_faceStreamPanelRefreshQueued = false;
    QTimer* m_faceStreamPanelRefreshTimer = nullptr;
    QString m_speakersTableRefreshSignature;
    QString m_faceStreamPanelRefreshSignature;
    QJsonArray m_faceStreamPanelRows;
    mutable QHash<QString, QString> m_avatarHoverTooltipHtmlCache;
    qint64 m_lastSpeakersTableRefreshDurationMs = 0;
    qint64 m_maxSpeakersTableRefreshDurationMs = 0;
    qint64 m_lastFaceStreamPanelRefreshDurationMs = 0;
    qint64 m_maxFaceStreamPanelRefreshDurationMs = 0;
    qint64 m_lastRawDetectionsPanelRefreshDurationMs = 0;
    qint64 m_maxRawDetectionsPanelRefreshDurationMs = 0;
    QFutureWatcher<TranscriptDocumentLoadResult> m_transcriptLoadWatcher;
    QString m_pendingPreferredSpeakerId;
};
