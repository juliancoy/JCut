#include "properties_tab.h"

#include <QDir>
#include <QSignalBlocker>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>

PropertiesTab::PropertiesTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void PropertiesTab::disableTrackControls()
{
    if (m_widgets.trackNameEdit) {
        QSignalBlocker blocker(m_widgets.trackNameEdit);
        m_widgets.trackNameEdit->setText(QString());
        m_widgets.trackNameEdit->setEnabled(false);
    }
    if (m_widgets.trackHeightSpin) {
        QSignalBlocker blocker(m_widgets.trackHeightSpin);
        m_widgets.trackHeightSpin->setValue(44);
        m_widgets.trackHeightSpin->setEnabled(false);
    }
    if (m_widgets.trackVisualModeCombo) {
        QSignalBlocker blocker(m_widgets.trackVisualModeCombo);
        m_widgets.trackVisualModeCombo->setCurrentIndex(0);
        m_widgets.trackVisualModeCombo->setEnabled(false);
    }
    if (m_widgets.trackAudioEnabledCheckBox) {
        QSignalBlocker blocker(m_widgets.trackAudioEnabledCheckBox);
        m_widgets.trackAudioEnabledCheckBox->setChecked(false);
        m_widgets.trackAudioEnabledCheckBox->setEnabled(false);
    }
    if (m_widgets.trackCrossfadeSecondsSpin) {
        m_widgets.trackCrossfadeSecondsSpin->setEnabled(false);
    }
    if (m_widgets.trackCrossfadeButton) {
        m_widgets.trackCrossfadeButton->setEnabled(false);
    }
}

void PropertiesTab::refresh()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const TimelineTrack* track = m_deps.getSelectedTrack ? m_deps.getSelectedTrack() : nullptr;
    const int selectedTrackIndex = m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1;

    if (!clip) {
        if (m_widgets.clipInspectorClipLabel) {
            m_widgets.clipInspectorClipLabel->setText(
                track ? QStringLiteral("No clip selected") : QStringLiteral("No clip or track selected"));
        }
        if (m_widgets.clipProxyUsageLabel) {
            m_widgets.clipProxyUsageLabel->setText(QStringLiteral("Proxy In Use: No"));
        }
        if (m_widgets.clipPlaybackSourceLabel) {
            m_widgets.clipPlaybackSourceLabel->setText(QStringLiteral("Playback Source: None"));
        }
        if (m_widgets.clipOriginalInfoLabel) {
            m_widgets.clipOriginalInfoLabel->setText(QStringLiteral("Original\nNo clip selected."));
        }
        if (m_widgets.clipProxyInfoLabel) {
            m_widgets.clipProxyInfoLabel->setText(QStringLiteral("Proxy\nNo proxy configured."));
        }
        if (m_widgets.clipPlaybackRateSpin) {
            QSignalBlocker block(m_widgets.clipPlaybackRateSpin);
            m_widgets.clipPlaybackRateSpin->setValue(1.0);
            m_widgets.clipPlaybackRateSpin->setEnabled(false);
        }
    } else {
        const QString proxyPath = m_deps.playbackProxyPathForClip ? m_deps.playbackProxyPathForClip(*clip) : QString();
        const QString playbackPath = m_deps.playbackMediaPathForClip ? m_deps.playbackMediaPathForClip(*clip)
                                                                     : clip->filePath;

        MediaProbeResult originalProbe;
        originalProbe.mediaType = clip->mediaType;
        originalProbe.sourceKind = clip->sourceKind;
        originalProbe.hasAudio = clip->hasAudio;
        originalProbe.hasVideo = clipHasVisuals(*clip);
        originalProbe.durationFrames = clip->sourceDurationFrames > 0 ? clip->sourceDurationFrames : clip->durationFrames;

        if (m_widgets.clipInspectorClipLabel) {
            m_widgets.clipInspectorClipLabel->setText(QStringLiteral("%1\n%2")
                .arg(clip->label,
                     QStringLiteral("%1 | %2")
                         .arg(clipMediaTypeLabel(clip->mediaType), mediaSourceKindLabel(clip->sourceKind))));
        }
        if (m_widgets.clipProxyUsageLabel) {
            m_widgets.clipProxyUsageLabel->setText(QStringLiteral("Proxy In Use: %1")
                .arg(playbackPath != clip->filePath ? QStringLiteral("Yes") : QStringLiteral("No")));
        }
        if (m_widgets.clipPlaybackSourceLabel) {
            if (clip->mediaType == ClipMediaType::Title) {
                m_widgets.clipPlaybackSourceLabel->setText(
                    QStringLiteral("Playback Source\nInline title overlay (no source file)"));
            } else {
                m_widgets.clipPlaybackSourceLabel->setText(QStringLiteral("Playback Source\n%1")
                    .arg(QDir::toNativeSeparators(playbackPath)));
            }
        }
        if (m_widgets.clipPlaybackRateSpin) {
            QSignalBlocker block(m_widgets.clipPlaybackRateSpin);
            m_widgets.clipPlaybackRateSpin->setValue(clip->playbackRate);
            m_widgets.clipPlaybackRateSpin->setEnabled(clipHasVisuals(*clip));
            m_widgets.clipPlaybackRateSpin->setToolTip(
                clip->hasAudio
                    ? QStringLiteral("Visual retime control. Audio playback is not time-stretched.")
                    : QStringLiteral("Playback speed multiplier for this clip."));
        }

        if (m_widgets.clipOriginalInfoLabel) {
            if (clip->mediaType == ClipMediaType::Title) {
                m_widgets.clipOriginalInfoLabel->setText(
                    QStringLiteral("Original\nTitle clips are generated overlays and do not have an associated media file."));
            } else {
                const QString summary = m_deps.clipFileInfoSummaryForClip
                    ? m_deps.clipFileInfoSummaryForClip(*clip, &originalProbe)
                    : clip->filePath;
                m_widgets.clipOriginalInfoLabel->setText(QStringLiteral("Original\n%1").arg(summary));
            }
        }

        if (m_widgets.clipProxyInfoLabel) {
            if (clip->mediaType == ClipMediaType::Title) {
                m_widgets.clipProxyInfoLabel->setText(QStringLiteral("Proxy\nNot applicable for title clips."));
            } else if (proxyPath.isEmpty()) {
                const QString configuredProxyPath = clip->proxyPath.isEmpty()
                    ? (m_deps.defaultProxyOutputPath ? m_deps.defaultProxyOutputPath(*clip) : QString())
                    : clip->proxyPath;
                m_widgets.clipProxyInfoLabel->setText(QStringLiteral("Proxy\nConfigured: No\nPath: %1")
                    .arg(QDir::toNativeSeparators(configuredProxyPath)));
            } else {
                const QString summary = m_deps.clipFileInfoSummaryForPath
                    ? m_deps.clipFileInfoSummaryForPath(proxyPath)
                    : proxyPath;
                m_widgets.clipProxyInfoLabel->setText(QStringLiteral("Proxy\n%1").arg(summary));
            }
        }
    }

    if (!track || selectedTrackIndex < 0 || clip) {
        if (m_widgets.trackInspectorLabel) {
            m_widgets.trackInspectorLabel->setText(QStringLiteral("No track selected"));
        }
        if (m_widgets.trackInspectorDetailsLabel) {
            m_widgets.trackInspectorDetailsLabel->setText(
                QStringLiteral("Select a track header to edit track-wide properties."));
        }
        disableTrackControls();
        return;
    }

    int clipCount = 0;
    int visualCount = 0;
    int audioCount = 0;
    bool allAudioEnabled = true;
    const QVector<TimelineClip> allClips = m_deps.getClips ? m_deps.getClips() : QVector<TimelineClip>{};
    for (const TimelineClip& timelineClip : allClips) {
        if (timelineClip.trackIndex != selectedTrackIndex) {
            continue;
        }
        ++clipCount;
        if (clipHasVisuals(timelineClip)) {
            ++visualCount;
        }
        if (timelineClip.hasAudio) {
            ++audioCount;
            allAudioEnabled = allAudioEnabled && timelineClip.audioEnabled;
        }
    }

    if (m_widgets.trackInspectorLabel) {
        m_widgets.trackInspectorLabel->setText(QStringLiteral("Track %1\n%2")
            .arg(selectedTrackIndex + 1)
            .arg(track->name));
    }
    if (m_widgets.trackInspectorDetailsLabel) {
        m_widgets.trackInspectorDetailsLabel->setText(QStringLiteral("%1 clips | %2 visual | %3 audio")
            .arg(clipCount)
            .arg(visualCount)
            .arg(audioCount));
    }
    if (m_widgets.trackNameEdit) {
        QSignalBlocker blocker(m_widgets.trackNameEdit);
        m_widgets.trackNameEdit->setText(track->name);
        m_widgets.trackNameEdit->setEnabled(true);
    }
    if (m_widgets.trackHeightSpin) {
        QSignalBlocker blocker(m_widgets.trackHeightSpin);
        m_widgets.trackHeightSpin->setValue(track->height);
        m_widgets.trackHeightSpin->setEnabled(true);
    }
    if (m_widgets.trackVisualModeCombo) {
        QSignalBlocker blocker(m_widgets.trackVisualModeCombo);
        const int modeIndex = m_widgets.trackVisualModeCombo->findData(static_cast<int>(track->visualMode));
        m_widgets.trackVisualModeCombo->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
        m_widgets.trackVisualModeCombo->setEnabled(visualCount > 0);
    }
    if (m_widgets.trackAudioEnabledCheckBox) {
        QSignalBlocker blocker(m_widgets.trackAudioEnabledCheckBox);
        m_widgets.trackAudioEnabledCheckBox->setChecked(audioCount > 0 ? allAudioEnabled : false);
        m_widgets.trackAudioEnabledCheckBox->setEnabled(audioCount > 0);
    }
    if (m_widgets.trackCrossfadeSecondsSpin) {
        m_widgets.trackCrossfadeSecondsSpin->setEnabled(clipCount > 1);
    }
    if (m_widgets.trackCrossfadeButton) {
        m_widgets.trackCrossfadeButton->setEnabled(clipCount > 1);
    }
}
