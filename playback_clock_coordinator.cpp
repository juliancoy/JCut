#include "playback_clock_coordinator.h"

#include "editor_shared.h"

#include <QtGlobal>

#include <cmath>

namespace editor {

int64_t audioMasterClockSampleToTimelineSample(int64_t audioClockSample)
{
    return qMax<int64_t>(0, audioClockSample);
}

PlaybackClockDecision evaluatePlaybackClock(const PlaybackClockInput& input)
{
    PlaybackClockDecision decision;

    if (input.pitchPreservingAudioRequired && (input.audioBlocked || !input.audioReady)) {
        decision.action = PlaybackClockAction::HoldForPitchPreservingAudio;
        decision.reason = input.audioBlocked
                              ? QStringLiteral("audio_blocked")
                              : QStringLiteral("retimed_audio_not_ready");
        decision.sample = qMax<int64_t>(0, input.audioSample);
        decision.resetTimerContinuity = true;
        return decision;
    }

    if (!input.audioMasterEnabled || !input.audioClockAvailable || !input.hasPlayableAudio) {
        decision.action = PlaybackClockAction::UseTransportSample;
        decision.reason = QStringLiteral("monotonic_transport");
        decision.sample = qMax<int64_t>(0, input.transportSample);
        decision.frame = qBound<int64_t>(
            0,
            static_cast<int64_t>(std::floor(samplesToFramePosition(decision.sample))),
            input.totalFrames);
        return decision;
    }

    const qreal audioFramePosition = samplesToFramePosition(qMax<int64_t>(0, input.audioSample));
    const int64_t audioFrame = qBound<int64_t>(
        0,
        static_cast<int64_t>(std::floor(audioFramePosition)),
        input.totalFrames);
    const int64_t currentFrame = qBound<int64_t>(0, input.currentFrame, input.totalFrames);
    decision.sample = qMax<int64_t>(0, input.audioSample);
    decision.frame = audioFrame;

    if (audioFrame + 2 < currentFrame) {
        decision.resetTimerContinuity = true;
        if (input.pitchPreservingAudioRequired) {
            decision.action = PlaybackClockAction::HoldForPitchPreservingAudio;
            decision.reason = QStringLiteral("audio_clock_regressed");
            return decision;
        }
        decision.action = PlaybackClockAction::UseAudioSample;
        decision.reason = QStringLiteral("audio_master_resync");
        return decision;
    }

    if (audioFrame == currentFrame) {
        decision.action = PlaybackClockAction::WaitForAudioClock;
        decision.reason = input.audioClockStallTicks <= input.audioClockStallThresholdTicks
                              ? QStringLiteral("audio_clock_same_frame")
                              : QStringLiteral("audio_clock_stalled");
        return decision;
    }

    decision.action = PlaybackClockAction::UseAudioSample;
    decision.reason = QStringLiteral("audio_master");
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
