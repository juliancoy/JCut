#pragma once

#include "editor_shared.h"

#include <QString>
#include <QVector>

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
    qreal deadbandSamples = 240.0;
    qreal fullCorrectionSamples = 9600.0;
    qreal maxCorrection = 0.08;
    qreal smoothing = 0.30;
};

int64_t audioFeedbackSampleToTimelineSample(int64_t audioFeedbackSample);
int64_t projectAudioFeedbackSampleToTimelineSample(
    int64_t audioFeedbackSample,
    int64_t anchorTimelineSample,
    int64_t anchorFeedbackSample,
    const QVector<ExportRangeSegment>& ranges);
PlaybackClockDecision evaluatePlaybackClock(const PlaybackClockInput& input);
qreal evaluatePlaybackDriftRetimeMultiplier(const PlaybackDriftRetimeInput& input);

}  // namespace editor
