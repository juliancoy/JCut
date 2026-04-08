#pragma once

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QVector>
#include <functional>

#include "editor_shared.h"

class ClipsTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QTableWidget* clipsTable = nullptr;
    };

    struct Dependencies {
        std::function<QVector<TimelineClip>()> getClips;
        std::function<QVector<TimelineTrack>()> getTracks;
        std::function<bool(const QString&)> deleteClipById;
        std::function<void(const QString&)> selectClipId;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<bool(int, const std::function<void(TimelineTrack&)>&)> updateTrackByIndex;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> scheduleSaveState;
    };

    explicit ClipsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~ClipsTab() override = default;

    void wire();
    void refresh();

private slots:
    void onCustomContextMenuRequested(const QPoint& pos);
    void onTableItemChanged(QTableWidgetItem* item);

private:
    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;

    static QColor trackColor(int trackIndex);
};
