#pragma once

#include <QWidget>
#include <QGridLayout>

class TimelineWidget;
class TrackSidebar;
class QLabel;
class QPushButton;
class QToolButton;
class QSlider;
class QSplitter;

// Container that holds TrackSidebar and TimelineWidget in a 2x2 grid:
// +------------------+------------------+
// | Placeholder      | Transport        |
// +------------------+------------------+
// | Track Sidebar    | Timeline         |
// +------------------+------------------+
class TimelineContainer final : public QWidget {
    Q_OBJECT

public:
    explicit TimelineContainer(QWidget *parent = nullptr);

    TimelineWidget *timeline() const { return m_timeline; }
    TrackSidebar *trackSidebar() const { return m_trackSidebar; }
    
    // Access to transport controls
    QPushButton *playButton() const;
    QToolButton *startButton() const;
    QToolButton *endButton() const;
    QSlider *seekSlider() const;
    QLabel *timecodeLabel() const;

    void syncTracksFromTimeline();

private slots:
    void onTrackVisualToggled(int trackIndex, bool enabled);
    void onTrackAudioToggled(int trackIndex, bool enabled);
    void onTrackSelected(int trackIndex);
    void onTrackMoveUp(int trackIndex);
    void onTrackMoveDown(int trackIndex);

private:
    void setupLayout();
    void connectSignals();

    // 2x2 Grid layout
    QGridLayout *m_gridLayout = nullptr;
    
    // Top row widgets
    QWidget *m_topLeftPlaceholder = nullptr;
    QWidget *m_transportWidget = nullptr;
    
    // Bottom row widgets  
    QSplitter *m_bottomSplitter = nullptr;
    TrackSidebar *m_trackSidebar = nullptr;
    TimelineWidget *m_timeline = nullptr;
};
