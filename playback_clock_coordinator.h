#pragma once

#include <QString>

#include <cstdint>

namespace editor {

enum class PlaybackClockAction {
    UseTransportSample,
};

struct PlaybackClockInput {
    int64_t transportSample = 0;
    int64_t totalFrames = 0;
};

struct PlaybackClockDecision {
    PlaybackClockAction action = PlaybackClockAction::UseTransportSample;
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

int64_t audioFeedbackSampleToTimelineSample(int64_t audioFeedbackSample);
PlaybackClockDecision evaluatePlaybackClock(const PlaybackClockInput& input);
qreal evaluatePlaybackDriftRetimeMultiplier(const PlaybackDriftRetimeInput& input);

}  // namespace editor
