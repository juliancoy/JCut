#pragma once

#include "editor_playback_types.h"

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
};

struct PlaybackFrameCrossfade {
    bool active = false;
    int64_t secondaryTimelineFrame = -1;
    float secondaryOpacity = 0.0f;
};

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
            const float t = qBound(0.0f,
                                   static_cast<float>(offset + 1) /
                                       static_cast<float>(window * 2),
                                   0.5f);
            const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            result.active = true;
            result.secondaryTimelineFrame = next.startFrame + offset;
            result.secondaryOpacity = qBound(0.0f, s, 1.0f);
            return result;
        }
        if (frame >= next.startFrame && frame < next.startFrame + window) {
            const int64_t offset = frame - next.startFrame;
            const float t = qBound(0.5f,
                                   static_cast<float>(window + offset) /
                                       static_cast<float>(window * 2),
                                   1.0f);
            const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            result.active = true;
            result.secondaryTimelineFrame = windowStart + offset;
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
        float t = -1.0f;
        if (frame >= windowStart && frame <= current.endFrame) {
            const int64_t offset = frame - windowStart;
            t = qBound(0.0f,
                       static_cast<float>(offset + 1) / static_cast<float>(window * 2),
                       0.5f);
        } else if (frame > current.endFrame && frame < next.startFrame) {
            t = 0.5f;
        } else if (frame >= next.startFrame && frame < next.startFrame + window) {
            const int64_t offset = frame - next.startFrame;
            t = qBound(0.5f,
                       static_cast<float>(window + offset) / static_cast<float>(window * 2),
                       1.0f);
        } else {
            continue;
        }
        const float s = playbackFrameTransitionSmoothValue(t, timing.frameTransitionMode);
        result.active = true;
        result.timelineFramePosition =
            static_cast<qreal>(current.endFrame) +
            static_cast<qreal>(next.startFrame - current.endFrame) * static_cast<qreal>(s);
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
