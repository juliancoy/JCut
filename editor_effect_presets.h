#pragma once

#include "editor_shared.h"

TimelineClip makeSamMaskMatteClip(const TimelineClip& sourceClip);
TimelineClip makeAlternatingMotionBackgroundClip(const TimelineClip& sourceClip, int trackIndex);
TimelineClip makeSourceTilingClip(const TimelineClip& sourceClip, int trackIndex);
QVector<TimelineClip> makeSpeakerTitleClipsForTranscriptIntroductions(
    const TimelineClip& sourceClip,
    const QString& transcriptPath,
    const QVector<TranscriptSection>& sections,
    int trackIndex,
    int64_t titleDurationFrames = kTimelineFps * 3);
bool applyNewsLowerThirdFlyInPreset(TimelineClip& clip);

struct GeneratedClipPlacementResult {
    bool changed = false;
    int removedCount = 0;
    int insertedCount = 0;
    QString firstInsertedClipId;
};

GeneratedClipPlacementResult replaceGeneratedClipsForSource(
    QVector<TimelineClip>& timelineClips,
    QVector<TimelineTrack>& timelineTracks,
    const QString& sourceClipId,
    ClipRole generatedRole,
    QVector<TimelineClip> generatedClips,
    const QString& trackBaseName);
