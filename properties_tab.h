#pragma once

#include <QObject>
#include <functional>

#include "editor_shared.h"

class QLabel;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QPushButton;

class PropertiesTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* clipInspectorClipLabel = nullptr;
        QLabel* clipProxyUsageLabel = nullptr;
        QLabel* clipPlaybackSourceLabel = nullptr;
        QLabel* clipOriginalInfoLabel = nullptr;
        QLabel* clipProxyInfoLabel = nullptr;
        QDoubleSpinBox* clipPlaybackRateSpin = nullptr;
        QLabel* trackInspectorLabel = nullptr;
        QLabel* trackInspectorDetailsLabel = nullptr;
        QLineEdit* trackNameEdit = nullptr;
        QSpinBox* trackHeightSpin = nullptr;
        QComboBox* trackVisualModeCombo = nullptr;
        QCheckBox* trackAudioEnabledCheckBox = nullptr;
        QDoubleSpinBox* trackCrossfadeSecondsSpin = nullptr;
        QPushButton* trackCrossfadeButton = nullptr;
    };

    struct Dependencies {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<const TimelineTrack*()> getSelectedTrack;
        std::function<int()> getSelectedTrackIndex;
        std::function<QVector<TimelineClip>()> getClips;
        std::function<QString(const TimelineClip&)> playbackProxyPathForClip;
        std::function<QString(const TimelineClip&)> playbackMediaPathForClip;
        std::function<QString(const TimelineClip&, const MediaProbeResult*)> clipFileInfoSummaryForClip;
        std::function<QString(const QString&)> clipFileInfoSummaryForPath;
        std::function<QString(const TimelineClip&)> defaultProxyOutputPath;
    };

    explicit PropertiesTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~PropertiesTab() override = default;

    void refresh();

private:
    void disableTrackControls();

    Widgets m_widgets;
    Dependencies m_deps;
};
