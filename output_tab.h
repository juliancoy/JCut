#pragma once

#include <QObject>
#include <QSpinBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QJsonObject>
#include <QString>
#include <QSize>
#include <functional>
#include <QVector>

#include "editor_shared.h"
#include "render.h"

class OutputTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QSpinBox* outputWidthSpin = nullptr;
        QSpinBox* outputHeightSpin = nullptr;
        QSpinBox* exportStartSpin = nullptr;
        QSpinBox* exportEndSpin = nullptr;
        QComboBox* outputFormatCombo = nullptr;
        QLabel* outputRangeSummaryLabel = nullptr;
        QCheckBox* renderUseProxiesCheckBox = nullptr;
        QCheckBox* outputPlaybackCacheFallbackCheckBox = nullptr;
        QCheckBox* outputLeadPrefetchEnabledCheckBox = nullptr;
        QSpinBox* outputLeadPrefetchCountSpin = nullptr;
        QSpinBox* outputPlaybackWindowAheadSpin = nullptr;
        QSpinBox* outputVisibleQueueReserveSpin = nullptr;
        QSpinBox* outputPrefetchMaxQueueDepthSpin = nullptr;
        QSpinBox* outputPrefetchMaxInflightSpin = nullptr;
        QSpinBox* outputPrefetchMaxPerTickSpin = nullptr;
        QSpinBox* outputPrefetchSkipVisiblePendingThresholdSpin = nullptr;
        QSpinBox* outputDecoderLaneCountSpin = nullptr;
        QComboBox* outputDecodeModeCombo = nullptr;
        QSpinBox* autosaveIntervalMinutesSpin = nullptr;
        QSpinBox* autosaveMaxBackupsSpin = nullptr;
        QCheckBox* createImageSequenceCheckBox = nullptr;
        QComboBox* imageSequenceFormatCombo = nullptr;
        QPushButton* renderButton = nullptr;
    };

    struct Dependencies
    {
        std::function<bool()> hasTimeline;
        std::function<bool()> hasClips;
        std::function<int64_t()> totalFrames;
        std::function<int64_t()> exportStartFrame;
        std::function<int64_t()> exportEndFrame;
        std::function<QVector<ExportRangeSegment>()> effectivePlaybackRanges;
        std::function<void(int64_t, int64_t)> setExportRange;
        std::function<void(const QSize&)> setOutputSize;
        std::function<void()> stopPlayback;
        std::function<QVector<TimelineClip>()> getTimelineClips;
        std::function<QVector<TimelineTrack>()> getTimelineTracks;
        std::function<QVector<RenderSyncMarker>()> getRenderSyncMarkers;
        std::function<void(const RenderRequest&)> renderTimeline;
        std::function<QString()> lastRenderOutputPath;
        std::function<void(const QString&)> setLastRenderOutputPath;
        std::function<int()> autosaveIntervalMinutes;
        std::function<void(int)> setAutosaveIntervalMinutes;
        std::function<int()> autosaveMaxBackups;
        std::function<void(int)> setAutosaveMaxBackups;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
    };

    explicit OutputTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~OutputTab() override = default;

    void wire();
    void refresh();
    void applyRangeFromInspector();
    void renderFromInspector();

private slots:
    void onOutputWidthChanged(int value);
    void onOutputHeightChanged(int value);
    void onExportStartChanged(int value);
    void onExportEndChanged(int value);
    void onOutputFormatChanged(int index);
    void onRenderUseProxiesToggled(bool checked);
    void onOutputPlaybackCacheFallbackToggled(bool checked);
    void onOutputLeadPrefetchEnabledToggled(bool checked);
    void onOutputLeadPrefetchCountChanged(int value);
    void onOutputPlaybackWindowAheadChanged(int value);
    void onOutputVisibleQueueReserveChanged(int value);
    void onOutputPrefetchMaxQueueDepthChanged(int value);
    void onOutputPrefetchMaxInflightChanged(int value);
    void onOutputPrefetchMaxPerTickChanged(int value);
    void onOutputPrefetchSkipVisiblePendingThresholdChanged(int value);
    void onOutputDecoderLaneCountChanged(int value);
    void onOutputDecodeModeChanged(int index);
    void onAutosaveIntervalMinutesChanged(int value);
    void onAutosaveMaxBackupsChanged(int value);
    void onRenderClicked();

private:
    void updateRangeSummary();
    void updateRenderButtonState();

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
};
