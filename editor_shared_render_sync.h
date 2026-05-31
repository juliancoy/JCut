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
qreal normalizedPlaybackSpeed(qreal speed);
PlaybackAudioWarpMode normalizedPlaybackAudioWarpMode(qreal playbackSpeed, PlaybackAudioWarpMode mode);
qreal effectivePlaybackAudioWarpRate(qreal playbackSpeed, PlaybackAudioWarpMode mode);
bool shouldUseAudioMasterClock(PlaybackClockSource source,
                               PlaybackAudioWarpMode mode,
                               qreal playbackSpeed,
                               bool hasPlayableAudio);
bool pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode mode,
                                              qreal playbackSpeed,
                                              bool hasPlayableAudio);
bool shouldHoldForPitchPreservingAudio(PlaybackAudioWarpMode mode,
                                       qreal playbackSpeed,
                                       bool hasPlayableAudio,
                                       bool audioBlocked,
                                       bool audioReady);

int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers);
int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers);
int64_t sourceFrameForClipAtTimelineSample(const TimelineClip& clip,
                                           int64_t timelineSample,
                                           const QVector<RenderSyncMarker>& markers);
int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers);
int64_t transcriptFrameForClipAtTimelineSample(const TimelineClip& clip,
                                               int64_t timelineSample,
                                               const QVector<RenderSyncMarker>& markers);
