#pragma once

#include <QObject>
#include <QTableWidget>
#include <functional>

#include "editor_shared.h"

class TracksTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QTableWidget* tracksTable = nullptr;
    };

    struct Dependencies {
        std::function<QVector<TimelineTrack>()> getTracks;
        std::function<bool(int)> trackHasVisualClips;
        std::function<TrackVisualMode(int)> trackVisualMode;
        std::function<bool(int)> trackHasAudioClips;
        std::function<bool(int)> trackAudioEnabled;
        std::function<bool(int, TrackVisualMode)> updateTrackVisualMode;
        std::function<bool(int, bool)> updateTrackAudioEnabled;
        std::function<void()> refreshInspector;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
    };

    explicit TracksTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~TracksTab() override = default;

    void wire();
    void refresh();

private slots:
    void onTableItemChanged(QTableWidgetItem* item);

private:
    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
};
