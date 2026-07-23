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
// A sync-locked generated follower has no independent media clock. Resolve
// the clip whose trim/rate/marker identity owns timeline-to-source mapping.
// The returned reference is either `clip` or an element of `timelineClips`.
const TimelineClip& resolvedClipTimingSource(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips);
// Return the visual/effect owner with only its clock identity replaced by the
// resolved source owner. This lets generated followers keep their own grade,
// masks, corrections, and effect parameters while render-sync markers and
// playback timing are evaluated in the parent's domain.
TimelineClip clipWithResolvedTimingOwner(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips);
ClipFrameMapping clipFrameMappingForClock(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips,
    const RenderFrameClock& clock,
    const QVector<RenderSyncMarker>& markers);
// Resolve the source-frame request used by generated-mask single-frame
// previews. The generated child has no independent clock, so this must use the
// same parent-aware trim/rate/render-sync mapping as preview and export.
int64_t requestedSourceFrameForGeneratedMaskPreview(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips,
    qreal timelineFramePosition,
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
int64_t timelineFrameForClipTranscriptFrame(const TimelineClip& clip,
                                            int64_t transcriptFrame,
                                            const QVector<RenderSyncMarker>& markers);
