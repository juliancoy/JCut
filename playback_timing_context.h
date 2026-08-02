#pragma once

#include "editor_playback_types.h"
#include "speech_filter_fade.h"

#include <QString>
#include <QVector>

#include <cmath>

enum class PlaybackFrameTransitionMode {
    Cut = 0,
    Crossfade,
    SmoothStepSpeedThrough,
    SmootherStepSpeedThrough,
};

inline QString playbackFrameTransitionModeToString(PlaybackFrameTransitionMode mode)
{
    switch (mode) {
    case PlaybackFrameTransitionMode::Cut:
        return QStringLiteral("cut");
    case PlaybackFrameTransitionMode::Crossfade:
        return QStringLiteral("crossfade");
    case PlaybackFrameTransitionMode::SmoothStepSpeedThrough:
        return QStringLiteral("smoothStepSpeedThrough");
    case PlaybackFrameTransitionMode::SmootherStepSpeedThrough:
        return QStringLiteral("smootherStepSpeedThrough");
    }
    return QStringLiteral("cut");
}

inline QString playbackFrameTransitionModeLabel(PlaybackFrameTransitionMode mode)
{
    switch (mode) {
    case PlaybackFrameTransitionMode::Cut:
        return QStringLiteral("Cut");
    case PlaybackFrameTransitionMode::Crossfade:
        return QStringLiteral("Crossfade");
    case PlaybackFrameTransitionMode::SmoothStepSpeedThrough:
        return QStringLiteral("Smooth Step Speed Through");
    case PlaybackFrameTransitionMode::SmootherStepSpeedThrough:
        return QStringLiteral("Smoother Step Speed Through");
    }
    return QStringLiteral("Cut");
}

inline PlaybackFrameTransitionMode playbackFrameTransitionModeFromString(
    const QString& value,
    PlaybackFrameTransitionMode fallback = PlaybackFrameTransitionMode::Cut)
{
    const QString normalized = value.trimmed();
    if (normalized == QStringLiteral("cut")) {
        return PlaybackFrameTransitionMode::Cut;
    }
    if (normalized == QStringLiteral("crossfade")) {
        return PlaybackFrameTransitionMode::Crossfade;
    }
    if (normalized == QStringLiteral("smoothStepSpeedThrough")) {
        return PlaybackFrameTransitionMode::SmoothStepSpeedThrough;
    }
    if (normalized == QStringLiteral("smootherStepSpeedThrough")) {
        return PlaybackFrameTransitionMode::SmootherStepSpeedThrough;
    }
    return fallback;
}

struct PlaybackTimingContext {
    QVector<ExportRangeSegment> playbackRanges;
    PlaybackFrameTransitionMode frameTransitionMode = PlaybackFrameTransitionMode::Cut;
    bool frameCrossfadeEnabled = false;
    int frameCrossfadeFrames = 0;
    editor::speech::FadeMode speechFadeMode =
        editor::speech::FadeMode::JumpCut;
    int speechFadeSamples = 0;
    qreal speechCurveStrength = 1.0;
};

struct PlaybackFrameCrossfade {
    bool active = false;
    int64_t secondaryTimelineFrame = -1;
    float secondaryOpacity = 0.0f;
};

struct PlaybackTimelineFrameClocks {
    qreal transportTimelineFrame = 0.0;
    qreal visualTimelineFrame = 0.0;
};

inline int64_t upcomingNoncontiguousPlaybackRangeStart(
    qreal timelineFramePosition,
    const PlaybackTimingContext& timing,
    int leadFrames)
{
    if (timing.playbackRanges.size() < 2 || leadFrames < 0) {
        return -1;
    }

    const int64_t frame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(timelineFramePosition)));
    for (int i = 0; i + 1 < timing.playbackRanges.size(); ++i) {
        const ExportRangeSegment& current = timing.playbackRanges.at(i);
        const ExportRangeSegment& next = timing.playbackRanges.at(i + 1);
        if (frame < current.startFrame || frame > current.endFrame) {
            continue;
        }
        if (next.startFrame <= current.endFrame + 1) {
            return -1;
        }
        return current.endFrame - frame <= leadFrames ? next.startFrame : -1;
    }
    return -1;
}

inline PlaybackFrameCrossfade playbackFrameCrossfadeAtTimelineFrame(
    qreal timelineFramePosition,
    const PlaybackTimingContext& timing)
{
    PlaybackFrameCrossfade result;
    if ((timing.frameTransitionMode != PlaybackFrameTransitionMode::Crossfade &&
         !timing.frameCrossfadeEnabled) ||
        timing.frameCrossfadeFrames <= 0 ||
        timing.playbackRanges.size() < 2) {
        return result;
    }
    const int64_t frame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(timelineFramePosition)));
    const int requestedWindow = qMax(1, timing.frameCrossfadeFrames);
    for (int i = 0; i + 1 < timing.playbackRanges.size(); ++i) {
        const ExportRangeSegment& current = timing.playbackRanges.at(i);
        const ExportRangeSegment& next = timing.playbackRanges.at(i + 1);
        const int64_t currentLength = qMax<int64_t>(1, current.endFrame - current.startFrame + 1);
        const int64_t nextLength = qMax<int64_t>(1, next.endFrame - next.startFrame + 1);
        const int64_t window = qMax<int64_t>(
            1,
            qMin<int64_t>(requestedWindow, qMin<int64_t>(currentLength, nextLength)));
        const int64_t windowStart = current.endFrame - window + 1;
        if (frame >= windowStart && frame <= current.endFrame) {
            const int64_t offset = frame - windowStart;
            const int64_t transitionOrdinal = offset + 1;
            const float t = qBound(0.0f,
                                   static_cast<float>(transitionOrdinal) /
                                       static_cast<float>(window * 2 + 1),
                                   1.0f);
            const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            result.active = true;
            // Preserve the complete outgoing and incoming frame sequences. A
            // duration-preserving crossfade cannot advance both sequences in
            // both halves without rewinding them at the range boundary, so
            // hold the incoming boundary frame during the outgoing half.
            result.secondaryTimelineFrame = next.startFrame;
            result.secondaryOpacity = qBound(0.0f, s, 1.0f);
            return result;
        }
        if (frame >= next.startFrame && frame < next.startFrame + window) {
            const int64_t offset = frame - next.startFrame;
            const int64_t transitionOrdinal = window + offset + 1;
            const float t = qBound(0.5f,
                                   static_cast<float>(transitionOrdinal) /
                                       static_cast<float>(window * 2 + 1),
                                   1.0f);
            const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            result.active = true;
            // Mirror the outgoing half by holding its boundary frame while
            // the incoming sequence advances normally.
            result.secondaryTimelineFrame = current.endFrame;
            result.secondaryOpacity = qBound(0.0f, 1.0f - s, 1.0f);
            return result;
        }
    }
    return result;
}

struct PlaybackFrameSpeedThrough {
    bool active = false;
    qreal timelineFramePosition = 0.0;
};

inline float playbackFrameTransitionSmoothValue(float t, PlaybackFrameTransitionMode mode)
{
    t = qBound(0.0f, t, 1.0f);
    if (mode == PlaybackFrameTransitionMode::SmootherStepSpeedThrough) {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }
    return t * t * (3.0f - 2.0f * t);
}

inline PlaybackFrameSpeedThrough playbackFrameSpeedThroughAtTimelineFrame(
    qreal timelineFramePosition,
    const PlaybackTimingContext& timing)
{
    PlaybackFrameSpeedThrough result;
    result.timelineFramePosition = qMax<qreal>(0.0, timelineFramePosition);
    if ((timing.frameTransitionMode != PlaybackFrameTransitionMode::SmoothStepSpeedThrough &&
         timing.frameTransitionMode != PlaybackFrameTransitionMode::SmootherStepSpeedThrough) ||
        timing.frameCrossfadeFrames <= 0 ||
        timing.playbackRanges.size() < 2) {
        return result;
    }
    const int64_t frame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(timelineFramePosition)));
    const int requestedWindow = qMax(1, timing.frameCrossfadeFrames);
    for (int i = 0; i + 1 < timing.playbackRanges.size(); ++i) {
        const ExportRangeSegment& current = timing.playbackRanges.at(i);
        const ExportRangeSegment& next = timing.playbackRanges.at(i + 1);
        const int64_t gapFrames = next.startFrame - current.endFrame - 1;
        if (gapFrames <= 0) {
            continue;
        }
        const int64_t currentLength = qMax<int64_t>(1, current.endFrame - current.startFrame + 1);
        const int64_t nextLength = qMax<int64_t>(1, next.endFrame - next.startFrame + 1);
        const int64_t window = qMax<int64_t>(
            1,
            qMin<int64_t>(requestedWindow, qMin<int64_t>(currentLength, nextLength)));
        const int64_t windowStart = current.endFrame - window + 1;
        const int64_t windowEnd = next.startFrame + window - 1;
        int64_t transitionOrdinal = -1;
        if (frame >= windowStart && frame <= current.endFrame) {
            transitionOrdinal = frame - windowStart;
        } else if (frame > current.endFrame && frame < next.startFrame) {
            // Transport normally skips this gap. Keep the mapping defined for
            // seeks and diagnostics without introducing a second midpoint.
            result.active = true;
            result.timelineFramePosition =
                static_cast<qreal>(windowStart + windowEnd) * 0.5;
            return result;
        } else if (frame >= next.startFrame && frame < next.startFrame + window) {
            transitionOrdinal = window + (frame - next.startFrame);
        } else {
            continue;
        }
        const int64_t transitionIntervals = qMax<int64_t>(1, window * 2 - 1);
        const float t = qBound(
            0.0f,
            static_cast<float>(transitionOrdinal) /
                static_cast<float>(transitionIntervals),
            1.0f);
        const float s = playbackFrameTransitionSmoothValue(t, timing.frameTransitionMode);
        result.active = true;
        result.timelineFramePosition =
            static_cast<qreal>(windowStart) +
            static_cast<qreal>(windowEnd - windowStart) * static_cast<qreal>(s);
        return result;
    }
    return result;
}

inline qreal playbackVisualTimelineFramePosition(qreal timelineFramePosition,
                                                 const PlaybackTimingContext& timing)
{
    const PlaybackFrameSpeedThrough speedThrough =
        playbackFrameSpeedThroughAtTimelineFrame(timelineFramePosition, timing);
    return speedThrough.active ? speedThrough.timelineFramePosition : timelineFramePosition;
}

inline PlaybackTimelineFrameClocks playbackTimelineFrameClocks(
    qreal transportTimelineFrame,
    const PlaybackTimingContext& timing)
{
    PlaybackTimelineFrameClocks clocks;
    clocks.transportTimelineFrame = transportTimelineFrame;
    clocks.visualTimelineFrame =
        playbackVisualTimelineFramePosition(transportTimelineFrame, timing);
    return clocks;
}
