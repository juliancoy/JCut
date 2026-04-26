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
    QComboBox *previewModeCombo() const { return m_previewModeCombo; }
    QToolButton *audioToolsButton() const { return m_audioToolsButton; }
    QLabel *statusBadge() const { return m_statusBadge; }
    QLabel *previewInfo() const { return m_previewInfo; }
    QToolButton *razorButton() const { return m_razorButton; }
    QLabel *aiStatusLabel() const { return m_aiStatusLabel; }
    QComboBox *aiModelCombo() const { return m_aiModelCombo; }
    QPushButton *aiTranscribeButton() const { return m_aiTranscribeButton; }
    QPushButton *aiFindSpeakerNamesButton() const { return m_aiFindSpeakerNamesButton; }
    QPushButton *aiFindOrganizationsButton() const { return m_aiFindOrganizationsButton; }
    QPushButton *aiCleanAssignmentsButton() const { return m_aiCleanAssignmentsButton; }
    QPushButton *aiLoginButton() const { return m_aiLoginButton; }
    QPushButton *aiLogoutButton() const { return m_aiLogoutButton; }

signals:
    void playClicked();
    void startClicked();
    void endClicked();
    void prevFrameClicked();
    void nextFrameClicked();
    void seekValueChanged(int value);
    void playbackSpeedChanged(double speed);
    void previewModeChanged(const QString& mode);
    void audioToolsClicked();
    void aiModelChanged(const QString& model);
    void aiTranscribeClicked();
    void aiFindSpeakerNamesClicked();
    void aiFindOrganizationsClicked();
    void aiCleanAssignmentsClicked();
    void aiLoginClicked();
    void aiLogoutClicked();
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
    QComboBox *m_previewModeCombo = nullptr;
    QToolButton *m_audioToolsButton = nullptr;
    QToolButton *m_audioMuteButton = nullptr;
    QSlider *m_audioVolumeSlider = nullptr;
    QLabel *m_audioNowPlayingLabel = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_previewInfo = nullptr;
    QWidget *m_aiPanel = nullptr;
    QLabel *m_aiStatusLabel = nullptr;
    QComboBox *m_aiModelCombo = nullptr;
    QPushButton *m_aiTranscribeButton = nullptr;
    QPushButton *m_aiFindSpeakerNamesButton = nullptr;
    QPushButton *m_aiFindOrganizationsButton = nullptr;
    QPushButton *m_aiCleanAssignmentsButton = nullptr;
    QPushButton *m_aiLoginButton = nullptr;
    QPushButton *m_aiLogoutButton = nullptr;
};
