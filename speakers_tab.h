#pragma once

#include <QJsonDocument>
#include <QPixmap>
#include <QTableWidget>
#include <QHash>
#include <functional>

#include "editor_shared.h"
#include "table_tab_base.h"

class QLabel;
class QPushButton;

class SpeakersTab : public TableTabBase {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* speakersInspectorClipLabel = nullptr;
        QLabel* speakersInspectorDetailsLabel = nullptr;
        QTableWidget* speakersTable = nullptr;
        QPushButton* speakerSetReference1Button = nullptr;
        QPushButton* speakerSetReference2Button = nullptr;
        QPushButton* speakerPickReference1Button = nullptr;
        QPushButton* speakerPickReference2Button = nullptr;
        QPushButton* speakerPreviousSegmentButton = nullptr;
        QPushButton* speakerNextSegmentButton = nullptr;
        QPushButton* speakerClearReferencesButton = nullptr;
        QPushButton* speakerRunAutoTrackButton = nullptr;
        QLabel* speakerTrackingStatusLabel = nullptr;
    };

    struct Dependencies : public TableTabBase::Dependencies {
    };

    explicit SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~SpeakersTab() override = default;

    void wire();
    void refresh();
    bool handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm);

signals:
    void transcriptDocumentChanged();

private slots:
    void onSpeakersTableItemChanged(QTableWidgetItem* item);
    void onSpeakersTableItemClicked(QTableWidgetItem* item);
    void onSpeakersSelectionChanged();
    void onSpeakerSetReference1Clicked();
    void onSpeakerSetReference2Clicked();
    void onSpeakerPickReference1Clicked();
    void onSpeakerPickReference2Clicked();
    void onSpeakerPreviousSegmentClicked();
    void onSpeakerNextSegmentClicked();
    void onSpeakerClearReferencesClicked();
    void onSpeakerRunAutoTrackClicked();

private:
    bool clipSupportsTranscript(const TimelineClip& clip) const;
    bool activeCutMutable() const;
    QString originalTranscriptPathForClip(const QString& clipFilePath) const;
    QString selectedSpeakerId() const;
    QString speakerTrackingSummary(const QJsonObject& profile) const;
    void updateSpeakerTrackingStatusLabel();
    void refreshSpeakersTable(const QJsonObject& transcriptRoot);
    void seekToSpeakerFirstWord(const QString& speakerId);
    bool seekToSpeakerSegmentRelative(const QString& speakerId, int direction);
    int64_t firstSourceFrameForSpeaker(const QJsonObject& transcriptRoot, const QString& speakerId) const;
    QPixmap speakerAvatarForRow(const TimelineClip& clip,
                                const QJsonObject& transcriptRoot,
                                const QJsonObject& profile,
                                const QString& speakerId);
    QPixmap placeholderSpeakerAvatar(const QString& speakerId) const;
    bool saveSpeakerProfileEdit(int tableRow, int column, const QString& valueText);
    bool saveSpeakerTrackingReferenceAt(const QString& speakerId,
                                        int referenceIndex,
                                        int64_t frame,
                                        qreal xNorm,
                                        qreal yNorm);
    bool saveSpeakerTrackingReference(const QString& speakerId, int referenceIndex);
    bool clearSpeakerTrackingReferences(const QString& speakerId);

    Widgets m_widgets;
    QString m_loadedTranscriptPath;
    QString m_loadedClipFilePath;
    QJsonDocument m_loadedTranscriptDoc;
    QHash<QString, QPixmap> m_avatarCache;
    int m_pendingReferencePick = 0;
};
