#pragma once

#include <QString>

#include <cstdint>

namespace editor {

enum class PlaybackClockAction {
    HoldForPitchPreservingAudio,
    UseTransportSample,
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
    int64_t transportSample = 0;
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

struct PlaybackDriftRetimeInput {
    bool enabled = false;
    int64_t driftSamples = 0;
    qreal previousMultiplier = 1.0;
    qreal deadbandSamples = 2400.0;
    qreal fullCorrectionSamples = 24000.0;
    qreal maxCorrection = 0.02;
    qreal smoothing = 0.08;
};

int64_t audioMasterClockSampleToTimelineSample(int64_t audioClockSample);
PlaybackClockDecision evaluatePlaybackClock(const PlaybackClockInput& input);
qreal evaluatePlaybackDriftRetimeMultiplier(const PlaybackDriftRetimeInput& input);

}  // namespace editor
