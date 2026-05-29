#pragma once

#include <QJsonDocument>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPixmap>
#include <QTableWidget>
#include <QPoint>
#include <QHash>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <functional>

#include "editor_action_result.h"
#include "editor_playback_types.h"
#include "editor_timeline_types.h"
#include "speaker_section_selection_timing_service.h"
#include "table_tab_base.h"
#include "transcript_document_io.h"
#include "transcript_document_session.h"

class QLabel;
class QListWidget;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
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
        QCheckBox* speakerHideUnidentifiedCheckBox = nullptr;
        QCheckBox* speakerShowContiguousSectionsCheckBox = nullptr;
        QTableWidget* speakerSectionsTable = nullptr;
        QLabel* selectedSpeakerIdLabel = nullptr;
        QListWidget* selectedSpeakerFaceDetectionsList = nullptr;
        QListWidget* speakerPlayheadFaceDetectionsList = nullptr;
        QCheckBox* speakerShowPlayheadFaceDetectionsCheckBox = nullptr;
        QPushButton* selectedSpeakerPreviousSentenceButton = nullptr;
        QPushButton* selectedSpeakerNextSentenceButton = nullptr;
        QPushButton* selectedSpeakerNextSectionButton = nullptr;
        QPushButton* selectedSpeakerRandomSentenceButton = nullptr;
        QLabel* speakerCurrentSentenceLabel = nullptr;
        QPushButton* speakerRunAutoTrackButton = nullptr;
        QPushButton* speakerViewFacestreamButton = nullptr;
        QPushButton* speakerFacestreamSettingsButton = nullptr;
        QPushButton* speakerRefreshTrackAvatarsButton = nullptr;
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
        QSpinBox* speakerFramingCenterSmoothingFramesSpin = nullptr;
        QSpinBox* speakerFramingZoomSmoothingFramesSpin = nullptr;
        QComboBox* speakerFramingSmoothingModeCombo = nullptr;
        QDoubleSpinBox* speakerFramingSmoothingStrengthSpin = nullptr;
        QSpinBox* speakerFramingGapHoldFramesSpin = nullptr;
        QCheckBox* speakerApplyFramingToClipCheckBox = nullptr;
        QTableWidget* speakerFramingEnabledKeyframeTable = nullptr;
        QLabel* speakerClipFramingStatusLabel = nullptr;
        QLabel* speakerRefsChipLabel = nullptr;
        QLabel* speakerPointstreamChipLabel = nullptr;
        QPushButton* speakerTrackingChipButton = nullptr;
        QPushButton* speakerStabilizeChipButton = nullptr;
        QCheckBox* speakerShowFaceDetectionsBoxesCheckBox = nullptr;
        QTableWidget* speakerFaceDetectionsTable = nullptr;
        QPlainTextEdit* speakerFaceDetectionsDetailsEdit = nullptr;
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
        std::function<void(const QSet<int>&)> setPreviewAssignedFaceTrackIds;
        std::function<void(const QString&)> setPreviewMode;
        std::function<bool()> isPlaybackActive;
        std::function<bool(QString*)> ensureAiSession;
        std::function<void(const QStringList&)> exportSpeakersVideo;
        std::function<void(bool)> setAudioBackgroundDecodeSuppressed;
    };

    explicit SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~SpeakersTab() override = default;

    void wire();
    void refresh();
    void refreshForSubtab(const QString& subtabName);
    void flushDeferredPlaybackRefreshes();
    void syncIdentityToPlayhead(int64_t absolutePlaybackSample,
                                double sourceSeconds,
                                int64_t sourceFrame);
    void syncCurrentSpeakerSentenceToPlayhead(bool duringPlayback = false);
    bool generateFaceDetectionsForSelectedClip();
    bool deleteFaceDetectionsForSelectedClip(bool confirmDialog = true,
                                         QString* errorOut = nullptr);
    editor::ActionResult deleteFaceDetectionsForSelectedClipResult(bool confirmDialog = true,
                                                               bool interactive = true);
    bool handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm);
    bool handlePreviewBox(const QString& clipId, qreal xNorm, qreal yNorm, qreal boxSizeNorm);
    bool handlePreviewFaceDetectionsBox(const QString& clipId,
                                    int trackId,
                                    const QString& streamId,
                                    int64_t sourceFrame,
                                    qreal xNorm,
                                    qreal yNorm,
                                    qreal boxSizeNorm);
    void showPreviewFaceDetectionsClickStatus(const QString& message);
    bool runAiFindSpeakerNames();
    bool runAiFindOrganizations();
    bool runAiCleanSpuriousAssignments();
    bool rebuildProcessedFaceDetectionsForSelectedClip(bool interactive = true);
    qint64 lastSpeakersTableRefreshDurationMs() const { return m_lastSpeakersTableRefreshDurationMs; }
    qint64 maxSpeakersTableRefreshDurationMs() const { return m_maxSpeakersTableRefreshDurationMs; }
    qint64 lastFaceDetectionsPanelRefreshDurationMs() const { return m_lastFaceDetectionsPanelRefreshDurationMs; }
    qint64 maxFaceDetectionsPanelRefreshDurationMs() const { return m_maxFaceDetectionsPanelRefreshDurationMs; }
    qint64 lastPlayheadTrackCandidatesRefreshDurationMs() const { return m_lastPlayheadTrackCandidatesRefreshDurationMs; }
    qint64 maxPlayheadTrackCandidatesRefreshDurationMs() const { return m_maxPlayheadTrackCandidatesRefreshDurationMs; }
    int lastPlayheadTrackCandidateCount() const { return m_lastPlayheadTrackCandidateCount; }
    qint64 lastRawDetectionsPanelRefreshDurationMs() const { return m_lastRawDetectionsPanelRefreshDurationMs; }
    qint64 maxRawDetectionsPanelRefreshDurationMs() const { return m_maxRawDetectionsPanelRefreshDurationMs; }
    QJsonObject speakerSectionSelectionTimingProfile() const { return m_sectionSelectionTiming.profileSnapshot(); }

signals:
    void transcriptDocumentChanged();

private slots:
    void onSpeakersTableItemChanged(QTableWidgetItem* item);
    void onSpeakersTableItemClicked(QTableWidgetItem* item);
    void onSpeakersSelectionChanged();
    void onSpeakersTableContextMenuRequested(const QPoint& pos);
    void onSpeakerSectionsTableContextMenuRequested(const QPoint& pos);
    void onSpeakerPreviousSentenceClicked();
    void onSpeakerNextSentenceClicked();
    void onSpeakerNextSectionClicked();
    void onSpeakerRandomSentenceClicked();
    void onSpeakerRunAutoTrackClicked();
    void onSpeakerViewFaceDetectionsClicked();
    void onSpeakerFaceDetectionsSettingsClicked();
    void onSpeakerRefreshTrackAvatarsClicked();
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
    void onSpeakerFramingEnabledTableSelectionChanged();
    void onSpeakerFramingEnabledTableItemChanged(QTableWidgetItem* item);
    void onSpeakerFramingEnabledTableContextMenu(const QPoint& pos);
    void onSpeakerFaceDetectionsTableContextMenuRequested(const QPoint& pos);
    void onSpeakerFindMatchingTracksClicked();

private:
    bool updateLoadedTranscriptDocument(const std::function<bool(QJsonObject&)>& mutator,
                                        bool clearDerivedCaches = true);
    bool saveLoadedTranscriptDocument();
    void queueLoadedTranscriptDocumentSave();
    void startTranscriptLoadRequest(const QString& clipFilePath,
                                    const QString& transcriptPath,
                                    const QString& preferredSpeakerId);
    void applyLoadedTranscriptDocumentData(const TimelineClip& clip, const QString& preferredSpeakerId);
    void refreshTranscriptSpeakerViews(const QString& preferredSpeakerId = QString(),
                                       bool refreshTrackPanels = false);
    void requestRefreshFaceDetectionsPathsPanel();
    void refreshPlayheadTrackCandidatesList(const TimelineClip& clip, const QString& speakerId);
    void updatePlayheadTrackCandidatesVisibility();
    enum class SentenceNavAction {
        Previous,
        Next,
        NextSection,
        Random
    };

    void refreshFaceDetectionsPathsPanel();
    void refreshSpeakerSectionsTable(const QJsonObject& transcriptRoot);
    void syncSpeakerListMode();
    bool selectSpeakerRowById(const QString& speakerId);
    bool selectSpeakerSectionRowAtFrame(const QString& speakerId, int64_t sourceFrame);
    void refreshRawDetectionsPanel(const QJsonObject& continuityRoot);
    QPixmap faceStreamPreviewAvatar(const TimelineClip& clip,
                                    const QString& speakerId,
                                    const QJsonObject& keyframeObj,
                                    int size = 72) const;
    QPixmap faceStreamPreviewAvatarWithDecoder(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& keyframeObj,
                                               int size,
                                               editor::DecoderContext* decoderCtx,
                                               QHash<int64_t, QImage>* frameImageCache) const;
    QJsonObject representativeKeyframeForTrack(const TimelineClip& clip,
                                               const QJsonObject& streamObj) const;
    QPixmap continuityTrackAvatar(const TimelineClip& clip,
                                  const QString& speakerId,
                                  const QJsonObject& streamObj,
                                  int size,
                                  editor::DecoderContext* decoderCtx = nullptr,
                                  QHash<int64_t, QImage>* frameImageCache = nullptr) const;
    QVector<QPixmap> assignedFaceDetectionsPreviewPixmaps(const TimelineClip& clip,
                                                      const QString& speakerId) const;
    QString assignedFaceDetectionsPreviewTooltipHtml(const TimelineClip& clip,
                                                 const QString& speakerId) const;
    QJsonArray continuityStreamsForClip(const TimelineClip& clip) const;
    void clearFaceDetectionsDerivedCaches();
    QJsonObject trackMemoryEntryForClip(const QString& clipId, int trackId) const;
    void ensurePersistentTrackAvatarMemory(const TimelineClip& clip,
                                           const QJsonArray& streams,
                                           bool forceRefresh = false,
                                           editor::DecoderContext* decoderCtx = nullptr,
                                           QHash<int64_t, QImage>* frameImageCache = nullptr);
    QJsonObject resolveFaceDetectionsAssignmentRow(const TimelineClip& clip,
                                               const QJsonArray& streams,
                                               const QJsonObject& row) const;
    struct TrackIdentityResolutionCache {
        QString signature;
        QHash<int, QString> identityByTrackId;
        QHash<QString, QVector<int>> trackIdsByIdentity;
    };
    const TrackIdentityResolutionCache& trackIdentityResolutionCacheForClip(
        const TimelineClip& clip,
        const QJsonArray& streams) const;
    QHash<int, QString> resolvedIdentityByTrackId(const TimelineClip& clip,
                                                  const QJsonArray& streams) const;
    QVector<int> resolvedAssignedTrackIdsForSpeaker(const TimelineClip& clip,
                                                    const QJsonArray& streams,
                                                    const QString& speakerId) const;
    void showSpeakerAvatarHoverPreview(const QString& speakerId, const QPoint& globalPos);
    void hideSpeakerAvatarHoverPreview();
    bool selectedClipHasFaceDetectionsSidecars() const;
    bool clipSupportsTranscript(const TimelineClip& clip) const;
    bool activeCutMutable() const;
    QString originalTranscriptPathForClip(const QString& clipFilePath) const;
    QString selectedSpeakerId() const;
    QString speakerDisplayName(const QString& speakerId) const;
    QString speakerDisplayLabel(const QString& speakerId) const;
    QString activeSpeakerIdAtSourceFrame(int64_t sourceFrame) const;
    QString activeSpeakerIdNearSourceFrame(int64_t sourceFrame,
                                           int gapHoldFrames,
                                           int64_t* resolvedSourceFrameOut = nullptr) const;
    bool ensureAiActionReady(const QString& actionTitle) const;
    void updateSpeakerTrackingStatusLabel();
    void updateSpeakerTrackingStatusLabelFast();
    void updateSelectedSpeakerPanel();
    void updateSelectedSpeakerPanelFast();
    void scheduleSelectedSpeakerPanelRefresh(int delayMs = 80);
    QString currentSpeakerSentenceAtCurrentFrame(const QString& speakerId) const;
    int64_t currentSourceFrameForClip(const TimelineClip& clip) const;
    void refreshSpeakersTable(const QJsonObject& transcriptRoot,
                              const QString& preferredSpeakerId = QString());
    void seekToSpeakerFirstWord(const QString& speakerId);
    bool seekToSpeakerSegmentRelative(const QString& speakerId, int direction);
    bool seekToSpeakerNextSection(const QString& speakerId);
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
                                QHash<int64_t, QImage>* frameImageCache = nullptr);
    QPixmap unsetSpeakerAvatar(int size) const;
    QPixmap placeholderSpeakerAvatar(const QString& speakerId) const;
    bool saveSpeakerProfileEdit(int tableRow, int column, const QString& valueText);
    bool deassignTrackFromSpeaker(const QString& speakerId, int trackId);
    bool deassignSelectedSpeakerAssignedTracks();
    void showSelectedSpeakerAssignedTracksContextMenu(const QPoint& pos);
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
    bool findMatchingTracksFromSeedTrack(int seedTrackId);
    bool openPlayheadTrackPickerForSpeaker(const QString& speakerId);
    QJsonObject makeTrackAssignmentAnchor(int trackId,
                                          const QString& streamId,
                                          int64_t sourceFrame,
                                          qreal xNorm,
                                          qreal yNorm,
                                          qreal boxSizeNorm) const;
    bool persistTrackAssignments(
        const QString& clipId,
        const QString& speakerId,
        const QJsonArray& trackAnchors,
        const QString& resolutionSource,
        const QString& auditAction,
        const QString& runIdPrefix,
        const std::function<bool(const QJsonObject&, const QJsonObject&)>& shouldReplaceResolvedRow,
        const std::function<bool(const QJsonObject&, const QJsonObject&)>& faceRefMatches);
    void openTrackPickerForSpeaker(const QString& speakerId);
    bool deleteSpeakerAutoTrackPointstream(const QString& speakerId);
    bool setSpeakerTrackingEnabled(const QString& speakerId, bool enabled);
    bool setSpeakerSkipped(const QString& speakerId, bool skipped);
    bool setSpeakerSectionSkipped(const QString& speakerId,
                                  int64_t startTimelineFrame,
                                  int64_t endTimelineFrame,
                                  bool skipped);
    bool saveClipSpeakerFramingTargetsFromControls();
    bool saveClipSpeakerFramingEnabledFromControls();
    void populateSpeakerFramingEnabledKeyframeTable(const TimelineClip& clip);
    void syncSpeakerFramingEnabledTableToPlayhead();
    bool upsertSpeakerFramingEnabledKeyframeAtPlayhead(bool enabled);
    bool removeSelectedSpeakerFramingEnabledKeyframes();
    void updateSpeakerFramingTargetControls();

    Widgets m_widgets;
    Dependencies m_speakerDeps;
    TranscriptDocumentSession m_transcriptSession{QStringLiteral("speakers")};
    mutable QHash<QString, QPixmap> m_avatarCache;
    mutable QHash<QString, QJsonArray> m_continuityStreamsCache;
    mutable QHash<QString, TrackIdentityResolutionCache> m_trackIdentityResolutionCache;
    QString m_lastSelectedSpeakerIdHint;
    bool m_updatingSpeakerFramingTargetControls = false;
    bool m_updatingSpeakerFramingEnabledTable = false;
    QSet<int64_t> m_selectedSpeakerFramingEnabledFrames;
    int64_t m_selectedSpeakerFramingEnabledFrame = -1;
    QString m_lastSelectionSeekSpeakerId;
    QString m_lastSelectionSeekClipId;
    bool m_skipNextPlayheadTrackCandidateRefresh = false;
    bool m_refreshingFaceDetectionsPathsPanel = false;
    bool m_faceStreamPanelRefreshQueued = false;
    bool m_assignmentHistorySnapshotQueued = false;
    bool m_selectedSpeakerPanelRefreshQueued = false;
    QTimer* m_faceStreamPanelRefreshTimer = nullptr;
    QTimer* m_selectedSpeakerPanelRefreshTimer = nullptr;
    QString m_speakersTableRefreshSignature;
    bool m_faceStreamPanelRefreshDeferredForPlayback = false;
    QString m_lastPlayheadSyncedSpeakerId;
    int64_t m_lastPlayheadSyncedSourceFrame = -1;
    QString m_faceStreamPanelRefreshSignature;
    QJsonArray m_faceStreamPanelRows;
    QElapsedTimer m_playbackSpeakerPanelThrottle;
    int64_t m_lastPlaybackSpeakerPanelSourceFrame = -1;
    QString m_lastPlaybackSpeakerPanelSpeakerId;
    mutable QHash<QString, QString> m_avatarHoverTooltipHtmlCache;
    qint64 m_lastSpeakersTableRefreshDurationMs = 0;
    qint64 m_maxSpeakersTableRefreshDurationMs = 0;
    qint64 m_lastFaceDetectionsPanelRefreshDurationMs = 0;
    qint64 m_maxFaceDetectionsPanelRefreshDurationMs = 0;
    qint64 m_lastPlayheadTrackCandidatesRefreshDurationMs = 0;
    qint64 m_maxPlayheadTrackCandidatesRefreshDurationMs = 0;
    qint64 m_lastRawDetectionsPanelRefreshDurationMs = 0;
    qint64 m_maxRawDetectionsPanelRefreshDurationMs = 0;
    SpeakerSectionSelectionTimingService m_sectionSelectionTiming;
    int m_lastPlayheadTrackCandidateCount = 0;
    QFutureWatcher<TranscriptDocumentLoadResult> m_transcriptLoadWatcher;
    QString m_pendingPreferredSpeakerId;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
