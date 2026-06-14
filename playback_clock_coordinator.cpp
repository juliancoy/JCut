#include "playback_clock_coordinator.h"

#include "editor_shared.h"

#include <QtGlobal>

#include <cmath>

namespace editor {

namespace {

int64_t filteredPlaybackSampleForAbsoluteSample(
    int64_t absoluteSample,
    const QVector<ExportRangeSegment>& ranges)
{
    if (ranges.isEmpty()) {
        return qMax<int64_t>(0, absoluteSample);
    }

    int64_t filteredSample = 0;
    for (const ExportRangeSegment& range : ranges) {
        const int64_t rangeStartSample = frameToSamples(range.startFrame);
        const int64_t rangeEndSampleExclusive = frameToSamples(range.endFrame + 1);
        if (absoluteSample <= rangeStartSample) {
            return filteredSample;
        }
        if (absoluteSample < rangeEndSampleExclusive) {
            return filteredSample + (absoluteSample - rangeStartSample);
        }
        filteredSample += (rangeEndSampleExclusive - rangeStartSample);
    }
    return filteredSample;
}

int64_t timelineSampleForFilteredPlaybackSample(
    int64_t filteredSample,
    const QVector<ExportRangeSegment>& ranges)
{
    if (ranges.isEmpty()) {
        return qMax<int64_t>(0, filteredSample);
    }

    int64_t remaining = qMax<int64_t>(0, filteredSample);
    for (const ExportRangeSegment& range : ranges) {
        const int64_t rangeStartSample = frameToSamples(range.startFrame);
        const int64_t rangeEndSampleExclusive = frameToSamples(range.endFrame + 1);
        const int64_t rangeLength = qMax<int64_t>(0, rangeEndSampleExclusive - rangeStartSample);
        if (remaining < rangeLength) {
            return rangeStartSample + remaining;
        }
        remaining -= rangeLength;
    }

    const ExportRangeSegment& last = ranges.constLast();
    return frameToSamples(last.endFrame + 1);
}

}  // namespace

int64_t audioFeedbackSampleToTimelineSample(int64_t audioFeedbackSample)
{
    return qMax<int64_t>(0, audioFeedbackSample);
}

int64_t projectAudioFeedbackSampleToTimelineSample(
    int64_t audioFeedbackSample,
    int64_t anchorTimelineSample,
    int64_t anchorFeedbackSample,
    const QVector<ExportRangeSegment>& ranges)
{
    const int64_t clampedFeedbackSample =
        audioFeedbackSampleToTimelineSample(audioFeedbackSample);
    const int64_t anchorTimeline = qMax<int64_t>(0, anchorTimelineSample);
    const int64_t anchorFeedback = qMax<int64_t>(0, anchorFeedbackSample);
    const int64_t audibleElapsedSamples =
        qMax<int64_t>(0, clampedFeedbackSample - anchorFeedback);

    if (ranges.isEmpty()) {
        return anchorTimeline + audibleElapsedSamples;
    }

    const int64_t anchorFilteredSample =
        filteredPlaybackSampleForAbsoluteSample(anchorTimeline, ranges);
    return timelineSampleForFilteredPlaybackSample(
        anchorFilteredSample + audibleElapsedSamples,
        ranges);
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
