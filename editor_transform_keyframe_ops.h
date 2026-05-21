#pragma once

#include "editor_shared.h"

#include <QList>
#include <QSet>

#include <cstdint>

int findTransformKeyframeIndex(const TimelineClip& clip, int64_t frame);
bool upsertStoredTransformKeyframe(TimelineClip& clip,
                                   const TimelineClip::TransformKeyframe& keyframe);
bool removeStoredTransformKeyframes(TimelineClip& clip, const QList<int64_t>& frames);
QSet<int64_t> duplicateStoredTransformKeyframesByDelta(TimelineClip& clip,
                                                       const QList<int64_t>& sourceFrames,
                                                       int64_t frameDelta);
bool duplicateStoredTransformKeyframesToFrame(TimelineClip& clip,
                                              const QList<int64_t>& sourceFrames,
                                              int64_t targetFrame);
TimelineClip::TransformKeyframe interpolateStoredTransformKeyframe(
    const TimelineClip::TransformKeyframe& earlier,
    const TimelineClip::TransformKeyframe& later,
    int64_t targetFrame);
