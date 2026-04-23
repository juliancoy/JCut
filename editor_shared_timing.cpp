#include "editor_shared.h"

#include <QtGlobal>

#include <cmath>

int64_t frameToSamples(int64_t frame) {
    return qMax<int64_t>(0, frame * kSamplesPerFrame);
}

qreal samplesToFramePosition(int64_t samples) {
    return static_cast<qreal>(samples) / static_cast<qreal>(kSamplesPerFrame);
}

qreal resolvedSourceFps(const TimelineClip& clip) {
    const qreal fps = clip.sourceFps;
    if (!std::isfinite(fps) || fps <= 0.001) {
        return static_cast<qreal>(kTimelineFps);
    }
    return fps;
}

int64_t sourceFramesToSamples(const TimelineClip& clip, qreal sourceFrames) {
    const qreal clampedSourceFrames = qMax<qreal>(0.0, sourceFrames);
    const qreal durationSeconds = clampedSourceFrames / resolvedSourceFps(clip);
    return qMax<int64_t>(0, qRound64(durationSeconds * static_cast<qreal>(kAudioSampleRate)));
}

int64_t clipTimelineStartSamples(const TimelineClip& clip) {
    return frameToSamples(clip.startFrame) + clip.startSubframeSamples;
}

int64_t clipSourceInSamples(const TimelineClip& clip) {
    return sourceFramesToSamples(clip, static_cast<qreal>(clip.sourceInFrame)) + clip.sourceInSubframeSamples;
}

void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples) {
    if (subframeSamples >= kSamplesPerFrame) {
        frame += subframeSamples / kSamplesPerFrame;
        subframeSamples %= kSamplesPerFrame;
    }
    while (subframeSamples < 0 && frame > 0) {
        --frame;
        subframeSamples += kSamplesPerFrame;
    }
    if (frame <= 0) {
        frame = 0;
        subframeSamples = qMax<int64_t>(0, subframeSamples);
    }
}

void normalizeClipTiming(TimelineClip& clip) {
    normalizeSubframeTiming(clip.startFrame, clip.startSubframeSamples);
    normalizeSubframeTiming(clip.sourceInFrame, clip.sourceInSubframeSamples);
    clip.playbackRate = qBound<qreal>(0.001, clip.playbackRate, 1000.0);
}

QString transformInterpolationLabel(bool linearInterpolation) {
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

