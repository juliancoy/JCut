#pragma once

#include "editor_shared.h"

#include <QList>
#include <QSet>

#include <cstdint>
#include <functional>

int findTitleKeyframeIndex(const TimelineClip& clip, int64_t frame);
bool upsertStoredTitleKeyframe(TimelineClip& clip,
                               const TimelineClip::TitleKeyframe& keyframe);
bool replaceStoredTitleKeyframeAtFrame(TimelineClip& clip,
                                       int64_t originalFrame,
                                       const TimelineClip::TitleKeyframe& keyframe);
bool updateStoredTitleKeyframeAtFrame(TimelineClip& clip,
                                      int64_t frame,
                                      const std::function<void(TimelineClip::TitleKeyframe&)>& mutate);
bool removeStoredTitleKeyframes(TimelineClip& clip, const QSet<int64_t>& frames);
TimelineClip::TitleKeyframe interpolateStoredTitleKeyframe(
    const TimelineClip::TitleKeyframe& earlier,
    const TimelineClip::TitleKeyframe& later,
    int64_t targetFrame);

int findOpacityKeyframeIndex(const TimelineClip& clip, int64_t frame);
bool upsertStoredOpacityKeyframe(TimelineClip& clip,
                                 const TimelineClip::OpacityKeyframe& keyframe);
bool replaceStoredOpacityKeyframeAtFrame(TimelineClip& clip,
                                         int64_t originalFrame,
                                         const TimelineClip::OpacityKeyframe& keyframe);
bool upsertOpacityKeyframePreservingInterpolation(TimelineClip& clip,
                                                  int64_t frame,
                                                  qreal opacity);
bool removeStoredOpacityKeyframes(TimelineClip& clip,
                                  const QList<int64_t>& frames,
                                  bool keepFrameZero = false);
