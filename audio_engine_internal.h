#pragma once

#include "audio_dynamics_core.h"
#include "editor_shared.h"

#include <algorithm>

namespace jcut::audio_internal {

inline constexpr qint64 kTimeStretchFailedJobRetryDelayMs = 10 * 60 * 1000;

inline bool anyAudioSolo(const QVector<TimelineClip>& clips,
                         const QVector<TimelineTrack>& tracks)
{
    return std::any_of(clips.cbegin(), clips.cend(), [](const TimelineClip& clip) {
               return clipAudioPlaybackEnabled(clip) && clip.audioSolo;
           }) || std::any_of(tracks.cbegin(), tracks.cend(), [](const TimelineTrack& track) {
               return track.audioSolo;
           });
}

inline bool audioDynamicsProcessingEnabled(
    const jcut::audio::DynamicsSettingsCore& settings)
{
    return settings.amplifyEnabled || settings.normalizeEnabled ||
        settings.selectiveNormalizeEnabled || settings.peakReductionEnabled ||
        settings.limiterEnabled || settings.compressorEnabled ||
        settings.softClipEnabled || settings.stereoToMonoEnabled;
}

inline jcut::audio::DynamicsSettingsCore audioDynamicsForClip(
    const TimelineClip& clip)
{
    return clip.audioDynamicsSet
        ? jcut::audio::normalizedDynamicsSettingsCore(clip.audioDynamics)
        : jcut::audio::DynamicsSettingsCore{};
}

inline float mixerGainForClip(const TimelineClip& clip,
                              const QVector<TimelineTrack>& tracks,
                              bool soloActive)
{
    if (!clipAudioPlaybackEnabled(clip)) {
        return 0.0f;
    }
    float gain = static_cast<float>(qBound<qreal>(0.0, clip.audioGain, 4.0));
    bool clipOrTrackSolo = clip.audioSolo;
    if (clip.trackIndex >= 0 && clip.trackIndex < tracks.size()) {
        const TimelineTrack& track = tracks.at(clip.trackIndex);
        if (!track.audioEnabled || track.audioMuted) {
            return 0.0f;
        }
        gain *= static_cast<float>(qBound<qreal>(0.0, track.audioGain, 4.0));
        clipOrTrackSolo = clipOrTrackSolo || track.audioSolo;
    }
    return soloActive && !clipOrTrackSolo ? 0.0f : gain;
}

} // namespace jcut::audio_internal
