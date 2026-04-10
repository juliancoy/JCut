#pragma once

#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QToolButton>
#include <QComboBox>
#include <QWidget>

class PreviewWindow;
class TimelineWidget;
class TimelineContainer;

class EditorPane final : public QWidget
{
    Q_OBJECT

public:
    explicit EditorPane(QWidget *parent = nullptr);

    PreviewWindow *previewWindow() const { return m_preview; }
    TimelineWidget *timelineWidget() const;
    TimelineContainer *timelineContainer() const { return m_timelineContainer; }

    QPushButton *playButton() const { return m_playButton; }
    QToolButton *audioMuteButton() const { return m_audioMuteButton; }
    QSlider *seekSlider() const { return m_seekSlider; }
    QSlider *audioVolumeSlider() const { return m_audioVolumeSlider; }
    QLabel *timecodeLabel() const { return m_timecodeLabel; }
    QLabel *audioNowPlayingLabel() const { return m_audioNowPlayingLabel; }
    QComboBox *playbackSpeedCombo() const { return m_playbackSpeedCombo; }
    QLabel *statusBadge() const { return m_statusBadge; }
    QLabel *previewInfo() const { return m_previewInfo; }
    QToolButton *razorButton() const { return m_razorButton; }

signals:
    void playClicked();
    void startClicked();
    void endClicked();
    void prevFrameClicked();
    void nextFrameClicked();
    void seekValueChanged(int value);
    void playbackSpeedChanged(double speed);
    void audioMuteClicked();
    void audioVolumeChanged(int value);

private:
    void setupTransportControls();
    PreviewWindow *m_preview = nullptr;
    TimelineContainer *m_timelineContainer = nullptr;

    QPushButton *m_playButton = nullptr;
    QToolButton *m_startButton = nullptr;
    QToolButton *m_endButton = nullptr;
    QToolButton *m_prevFrameButton = nullptr;
    QToolButton *m_nextFrameButton = nullptr;
    QToolButton *m_razorButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QLabel *m_timecodeLabel = nullptr;
    QComboBox *m_playbackSpeedCombo = nullptr;
    QToolButton *m_audioMuteButton = nullptr;
    QSlider *m_audioVolumeSlider = nullptr;
    QLabel *m_audioNowPlayingLabel = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_previewInfo = nullptr;
};
