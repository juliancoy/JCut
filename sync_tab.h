#pragma once

#include <QObject>
#include <QColor>
#include <QSet>
#include <QTimer>
#include <QTableWidget>
#include <functional>

#include "editor_shared.h"

class QLabel;
class QPushButton;

class SyncTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* syncInspectorClipLabel = nullptr;
        QLabel* syncInspectorDetailsLabel = nullptr;
        QTableWidget* syncTable = nullptr;
        QPushButton* clearAllSyncPointsButton = nullptr;
    };

    struct Dependencies {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<QVector<RenderSyncMarker>()> getRenderSyncMarkers;
        std::function<void(const QVector<RenderSyncMarker>&)> setRenderSyncMarkers;
        std::function<int64_t()> getCurrentTimelineFrame;
        std::function<void(int64_t)> seekToTimelineFrame;
        std::function<QString(const QString&)> clipLabelForId;
        std::function<QColor(const QString&)> clipColorForId;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<bool(int)> confirmClearAllSyncPoints;
        std::function<void()> showNoSyncPointsInfo;
    };

    explicit SyncTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~SyncTab() override = default;

    void wire();
    void refresh();

private slots:
    void onTableSelectionChanged();
    void onTableItemChanged(QTableWidgetItem* item);
    void onTableItemDoubleClicked(QTableWidgetItem* item);
    void onTableCustomContextMenu(const QPoint& pos);
    void onClearAllSyncPointsClicked();

private:
    static bool parseSyncActionText(const QString& text, RenderSyncAction* actionOut);
    void scheduleDeferredSeek(int64_t frame);
    void cancelDeferredSeek();

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
    QTimer m_deferredSeekTimer;
    int64_t m_pendingSeekFrame = -1;
};
