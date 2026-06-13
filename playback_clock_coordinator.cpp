#include "playback_clock_coordinator.h"

#include "editor_shared.h"

#include <QtGlobal>

#include <cmath>

namespace editor {

int64_t audioFeedbackSampleToTimelineSample(int64_t audioFeedbackSample)
{
    return qMax<int64_t>(0, audioFeedbackSample);
}

PlaybackClockDecision evaluatePlaybackClock(const PlaybackClockInput& input)
{
    PlaybackClockDecision decision;
    decision.action = PlaybackClockAction::UseTransportSample;
    decision.reason = QStringLiteral("system_clock_transport");
    decision.sample = qMax<int64_t>(0, input.transportSample);
    decision.frame = qBound<int64_t>(
        0,
        static_cast<int64_t>(std::floor(samplesToFramePosition(decision.sample))),
        input.totalFrames);
    return decision;
}

qreal evaluatePlaybackDriftRetimeMultiplier(const PlaybackDriftRetimeInput& input)
{
    const qreal correction = qMax<qreal>(0.0, input.maxCorrection);
    const qreal previous = qBound<qreal>(1.0 - correction,
                                         input.previousMultiplier,
                                         1.0 + correction);
    const qreal smoothing = qBound<qreal>(0.0, input.smoothing, 1.0);
    if (!input.enabled) {
        return previous + ((1.0 - previous) * smoothing);
    }

    const qreal deadband = qMax<qreal>(0.0, input.deadbandSamples);
    const qreal fullCorrection = qMax<qreal>(deadband + 1.0,
                                             input.fullCorrectionSamples);
    const qreal absDrift = qAbs(static_cast<qreal>(input.driftSamples));
    qreal target = 1.0;
    if (absDrift > deadband) {
        const qreal normalized = qBound<qreal>(
            0.0,
            (absDrift - deadband) / (fullCorrection - deadband),
            1.0);
        const qreal direction = input.driftSamples > 0 ? 1.0 : -1.0;
        target += direction * correction * normalized;
    }

    return qBound<qreal>(1.0 - correction,
                         previous + ((target - previous) * smoothing),
                         1.0 + correction);
}

}  // namespace editor
