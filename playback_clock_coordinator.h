#pragma once

#include <QString>

#include <cstdint>

namespace editor {

enum class PlaybackClockAction {
    HoldForPitchPreservingAudio,
    UseAudioSample,
    WaitForAudioClock,
    UseTimelineTimer,
};

struct PlaybackClockInput {
    bool pitchPreservingAudioRequired = false;
    bool audioMasterEnabled = false;
    bool audioClockAvailable = false;
    bool hasPlayableAudio = false;
    bool audioBlocked = false;
    bool audioReady = true;
    int64_t audioSample = 0;
    int64_t currentFrame = 0;
    int64_t totalFrames = 0;
    int audioClockStallTicks = 0;
    int audioClockStallThresholdTicks = 0;
};

struct PlaybackClockDecision {
    PlaybackClockAction action = PlaybackClockAction::UseTimelineTimer;
    QString reason;
    int64_t sample = 0;
    int64_t frame = 0;
    bool resetTimerContinuity = false;
};

PlaybackClockDecision evaluatePlaybackClock(const PlaybackClockInput& input);

}  // namespace editor
