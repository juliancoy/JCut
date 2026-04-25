#pragma once

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QTimer>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <functional>

#include "editor_shared.h"
#include "transcript_engine.h"

class TranscriptTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLineEdit* transcriptInspectorClipLabel = nullptr;
        QLabel* transcriptInspectorDetailsLabel = nullptr;
        QTableWidget* transcriptTable = nullptr;
        QCheckBox* transcriptOverlayEnabledCheckBox = nullptr;
        QCheckBox* transcriptBackgroundVisibleCheckBox = nullptr;
        QSpinBox* transcriptMaxLinesSpin = nullptr;
        QSpinBox* transcriptMaxCharsSpin = nullptr;
        QCheckBox* transcriptAutoScrollCheckBox = nullptr;
        QCheckBox* transcriptFollowCurrentWordCheckBox = nullptr;
        QDoubleSpinBox* transcriptOverlayXSpin = nullptr;
        QDoubleSpinBox* transcriptOverlayYSpin = nullptr;
        QSpinBox* transcriptOverlayWidthSpin = nullptr;
        QSpinBox* transcriptOverlayHeightSpin = nullptr;
        QFontComboBox* transcriptFontFamilyCombo = nullptr;
        QSpinBox* transcriptFontSizeSpin = nullptr;
        QCheckBox* transcriptBoldCheckBox = nullptr;
        QCheckBox* transcriptItalicCheckBox = nullptr;
        QSpinBox* transcriptPrependMsSpin = nullptr;
        QSpinBox* transcriptPostpendMsSpin = nullptr;
        QCheckBox* speechFilterEnabledCheckBox = nullptr;
        QSpinBox* speechFilterFadeSamplesSpin = nullptr;
        QCheckBox* transcriptUnifiedEditModeCheckBox = nullptr;
        QComboBox* transcriptSpeakerFilterCombo = nullptr;
        QComboBox* transcriptScriptVersionCombo = nullptr;
        QPushButton* transcriptNewVersionButton = nullptr;
        QPushButton* transcriptDeleteVersionButton = nullptr;
        QCheckBox* transcriptShowExcludedLinesCheckBox = nullptr;
    };

    struct Dependencies
    {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> refreshInspector;
        std::function<void()> setPreviewTimelineClips;
        std::function<QVector<ExportRangeSegment>()> effectivePlaybackRanges;
        std::function<void(int64_t)> seekToTimelineFrame;
    };

    explicit TranscriptTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~TranscriptTab() override = default;

    void wire();
    void refresh();
    void applyOverlayFromInspector(bool pushHistory = false);
    void syncTableToPlayhead(int64_t absolutePlaybackSample, double sourceSeconds);
    void applyTableEdit(QTableWidgetItem* item);
    void deleteSelectedRows();
    void setSelectedRowsSkipped(bool skipped);
    int transcriptPrependMs() const { return m_transcriptPrependMs; }
    int transcriptPostpendMs() const { return m_transcriptPostpendMs; }
    int speechFilterFadeSamples() const { return m_speechFilterFadeSamples; }
    bool speechFilterEnabled() const { return m_speechFilterEnabled; }
    void setManualSelectionHoldMs(int valueMs);
    int manualSelectionHoldMs() const { return m_manualSelectionHoldMs; }

signals:
    void transcriptDocumentChanged();
    void speechFilterParametersChanged();

private slots:
    void onTranscriptItemClicked(QTableWidgetItem* item);
    void onTranscriptItemDoubleClicked(QTableWidgetItem* item);
    void onTranscriptCustomContextMenu(const QPoint& pos);
    void onTranscriptHeaderContextMenu(const QPoint& pos);
    void onTranscriptSelectionChanged();
    void onFollowCurrentWordToggled(bool checked);
    void onOverlaySettingChanged();
    void onPrependMsChanged(int value);
    void onPostpendMsChanged(int value);
    void onSpeechFilterEnabledToggled(bool enabled);
    void onSpeechFilterFadeSamplesChanged(int value);
    void onTranscriptScriptVersionChanged(int index);
    void onTranscriptScriptVersionContextMenu(const QPoint& pos);
    void onTranscriptCutLabelEdited();
    void onTranscriptCreateVersion();
    void onTranscriptDeleteVersion();

private:
    struct TranscriptRow
    {
        enum EditFlag
        {
            EditNone = 0,
            EditTiming = 1 << 0,
            EditText = 1 << 1,
            EditSkip = 1 << 2,
            EditInserted = 1 << 3
        };

        int64_t startFrame = 0;
        int64_t endFrame = 0;
        int64_t renderStartFrame = 0;
        int64_t renderEndFrame = 0;
        QString speaker;
        QString text;
        bool isGap = false;
        bool isOutsideActiveCut = false;
        bool isSkipped = false;
        int editFlags = EditNone;
        int segmentIndex = -1;
        int wordIndex = -1;
        int originalSegmentIndex = -1;
        int originalWordIndex = -1;
    };
    struct FollowRange
    {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        int row = -1;
    };

    void updateOverlayWidgetsFromClip(const TimelineClip& clip);
    void loadTranscriptFile(const TimelineClip& clip);
    QVector<TranscriptRow> parseTranscriptRows(const QJsonArray& segments, int prependMs, int postpendMs);
    void populateTable(const QVector<TranscriptRow>& rows);
    void adjustOverlappingRows(QVector<TranscriptRow>& rows);
    void insertWordAtRow(int row, bool above);
    void expandSelectedRow(int row);
    void applyTranscriptRowState(QTableWidgetItem* startItem,
                                 QTableWidgetItem* endItem,
                                 QTableWidgetItem* speakerItem,
                                 QTableWidgetItem* textItem,
                                 QTableWidgetItem* editsItem,
                                 const TranscriptRow& entry) const;
    void refreshSpeakerFilter(const QVector<TranscriptRow>& rows);
    QString activeSpeakerFilter() const;
    QVector<TranscriptRow> filteredRowsForSpeaker(const QVector<TranscriptRow>& rows) const;
    QStringList editLabelsForFlags(int flags) const;
    bool unifiedEditColorsEnabled() const;
    void refreshScriptVersionSelector(const QString& clipFilePath, const QString& selectedPath);
    QStringList scriptVersionPathsForClip(const QString& clipFilePath) const;
    QString scriptVersionLabelForPath(const QString& path, const QString& clipFilePath) const;
    QString defaultEditablePathForClip(const QString& clipFilePath) const;
    QString originalTranscriptPathForClip(const QString& clipFilePath) const;
    QString nextScriptVersionPathForClip(const QString& clipFilePath) const;
    bool activeCutMutable() const;
    bool showOutsideCutLinesEnabled() const;
    QString originalWordKey(const TranscriptRow& row) const;
    void persistRenderOrderFromTable();
    void computeRenderFrames(QVector<TranscriptRow>* rows) const;
    void rebuildFollowRanges(const QVector<TranscriptRow>& rows);
    void scheduleSeekToTranscriptRow(int row);
    bool hasActiveManualSelection() const;
    bool eventFilter(QObject* watched, QEvent* event) override;

    Widgets m_widgets;
    Dependencies m_deps;
    editor::TranscriptEngine m_transcriptEngine;
    bool m_updating = false;
    QString m_loadedTranscriptPath;
    QString m_loadedClipFilePath;
    QJsonDocument m_loadedTranscriptDoc;
    int m_transcriptPrependMs = 150;
    int m_transcriptPostpendMs = 70;
    int m_speechFilterFadeSamples = 300;
    bool m_speechFilterEnabled = false;
    QTimer m_deferredSeekTimer;
    int64_t m_pendingSeekTimelineFrame = -1;
    int m_manualSelectionHoldMs = 1200;
    QElapsedTimer m_manualSelectionTimer;
    int64_t m_lastSyncAbsolutePlaybackSample = -1;
    double m_lastSyncSourceSeconds = -1.0;
    QVector<FollowRange> m_followRanges;
    bool m_suppressSelectionSideEffects = false;
    QString m_persistedSelectedClipId;
    int m_persistedSelectedSegmentIndex = -1;
    int m_persistedSelectedWordIndex = -1;
    bool m_updatingScriptVersionSelector = false;
};
