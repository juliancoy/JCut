#pragma once

#include "editor_shared_core.h"

QString renderSyncActionToString(RenderSyncAction action);
RenderSyncAction renderSyncActionFromString(const QString& value);
QString renderSyncActionLabel(RenderSyncAction action);
QString playbackClockSourceToString(PlaybackClockSource source);
PlaybackClockSource playbackClockSourceFromString(const QString& value);
QString playbackClockSourceLabel(PlaybackClockSource source);
QString playbackAudioWarpModeToString(PlaybackAudioWarpMode mode);
PlaybackAudioWarpMode playbackAudioWarpModeFromString(const QString& value);
QString playbackAudioWarpModeLabel(PlaybackAudioWarpMode mode);
bool playbackAudioWarpModeUsesTimeStretch(PlaybackAudioWarpMode mode);
bool playbackAudioWarpModeForcesUnityTimeStretch(PlaybackAudioWarpMode mode);
qreal normalizedPlaybackSpeed(qreal speed);
PlaybackAudioWarpMode normalizedPlaybackAudioWarpMode(qreal playbackSpeed, PlaybackAudioWarpMode mode);
qreal effectivePlaybackAudioWarpRate(qreal playbackSpeed, PlaybackAudioWarpMode mode);
bool pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode mode,
                                              qreal playbackSpeed,
                                              bool hasPlayableAudio);

struct RenderFrameClock {
    qreal timelineFramePosition = 0.0;
    int64_t timelineSample = 0;
    int64_t timelineFrame = 0;
};

struct ClipFrameMapping {
    RenderFrameClock clock;
    int64_t sourceSample = 0;
    qreal sourceFramePosition = 0.0;
    int64_t sourceFrame = 0;
    int64_t transcriptFrame = 0;
};

RenderFrameClock renderFrameClockForTimelinePosition(qreal timelineFramePosition);
RenderFrameClock renderFrameClockForTimelineSample(int64_t timelineSample);
ClipFrameMapping clipFrameMappingForClock(const TimelineClip& clip,
                                          const RenderFrameClock& clock,
                                          const QVector<RenderSyncMarker>& markers);
int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers);
int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers);
qreal sourceFramePositionForClipAtTimelinePosition(const TimelineClip& clip,
                                                  qreal timelineFramePosition,
                                                  const QVector<RenderSyncMarker>& markers);
qreal sourceFramePositionForClipAtTimelineSample(const TimelineClip& clip,
                                                 int64_t timelineSample,
                                                 const QVector<RenderSyncMarker>& markers);
int64_t approximateTimelineFrameForClipSourceFrame(const TimelineClip& clip,
                                                   int64_t sourceFrame);
int64_t sourceFrameForClipAtTimelineSample(const TimelineClip& clip,
                                           int64_t timelineSample,
                                           const QVector<RenderSyncMarker>& markers);
int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers);
int64_t transcriptFrameForClipSourceFrame(const TimelineClip& clip,
                                          int64_t mediaSourceFrame);
int64_t transcriptFrameForClipAtTimelineSample(const TimelineClip& clip,
                                               int64_t timelineSample,
                                               const QVector<RenderSyncMarker>& markers);
