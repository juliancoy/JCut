#pragma once

#include <QJsonDocument>
#include <QPixmap>
#include <QTableWidget>
#include <QHash>
#include <QPoint>
#include <QStringList>
#include <functional>

#include "editor_shared.h"
#include "table_tab_base.h"

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QCheckBox;

class SpeakersTab : public TableTabBase {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* speakersInspectorClipLabel = nullptr;
        QLabel* speakersInspectorDetailsLabel = nullptr;
        QTableWidget* speakersTable = nullptr;
        QLabel* selectedSpeakerIdLabel = nullptr;
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
        QPushButton* speakerBoxstreamSettingsButton = nullptr;
        QPushButton* speakerEnableTrackingButton = nullptr;
        QPushButton* speakerDisableTrackingButton = nullptr;
        QPushButton* speakerDeletePointstreamButton = nullptr;
        QPushButton* speakerGuideButton = nullptr;
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
    };

    struct Dependencies : public TableTabBase::Dependencies {
        std::function<QVector<RenderSyncMarker>()> getRenderSyncMarkers;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<QSize()> getOutputSize;
        std::function<void()> refreshPreview;
        std::function<void(const QStringList&)> exportSpeakersVideo;
    };

    explicit SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~SpeakersTab() override = default;

    void wire();
    void refresh();
    bool handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm);
    bool handlePreviewBox(const QString& clipId, qreal xNorm, qreal yNorm, qreal boxSizeNorm);
    bool runAiFindSpeakerNames();
    bool runAiFindOrganizations();
    bool runAiCleanSpuriousAssignments();

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
    void onSpeakerBoxStreamSettingsClicked();
    void onSpeakerEnableTrackingClicked();
    void onSpeakerDisableTrackingClicked();
    void onSpeakerDeletePointstreamClicked();
    void onSpeakerTrackingChipClicked();
    void onSpeakerStabilizeChipClicked();
    void onSpeakerGuideClicked();
    void onSpeakerFramingTargetChanged();
    void onSpeakerFramingZoomEnabledChanged(bool checked);
    void onSpeakerApplyFramingToClipChanged(bool checked);

private:
    enum class SentenceNavAction {
        Previous,
        Next,
        Random
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    bool clipSupportsTranscript(const TimelineClip& clip) const;
    bool activeCutMutable() const;
    QString originalTranscriptPathForClip(const QString& clipFilePath) const;
    QString selectedSpeakerId() const;
    QString speakerTrackingSummary(const QJsonObject& profile) const;
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
    bool runAutoTrackForSpeaker(const QString& speakerId, bool forceModelTracking = false);
    bool runNativeAutoTrackForSpeaker(const TimelineClip& clip,
                                      const QString& speakerId,
                                      const QJsonObject& ref1,
                                      const QJsonObject& ref2,
                                      const QVector<QPair<int64_t, int64_t>>& activeWindows,
                                      int64_t startFrame,
                                      int64_t endFrame,
                                      int stepFrames,
                                      QJsonArray* keyframesOut,
                                      QString* errorOut);
    bool runDockerAutoTrackForSpeaker(const TimelineClip& clip,
                                      const QString& speakerId,
                                      const QJsonObject& ref1,
                                      const QJsonObject& ref2,
                                      int64_t startFrame,
                                      int64_t endFrame,
                                      int stepFrames,
                                      QJsonArray* keyframesOut,
                                      QString* errorOut);
    bool navigateSpeakerSentence(const QString& speakerId, SentenceNavAction action);
    QVector<int64_t> speakerSourceFrames(const QJsonObject& transcriptRoot,
                                         const QString& speakerId) const;
    int64_t firstSourceFrameForSpeaker(const QJsonObject& transcriptRoot, const QString& speakerId) const;
    QPixmap speakerAvatarForRow(const TimelineClip& clip,
                                const QJsonObject& transcriptRoot,
                                const QJsonObject& profile,
                                const QString& speakerId);
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
    QString m_loadedTranscriptPath;
    QString m_loadedClipFilePath;
    QJsonDocument m_loadedTranscriptDoc;
    QHash<QString, QPixmap> m_avatarCache;
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
};
