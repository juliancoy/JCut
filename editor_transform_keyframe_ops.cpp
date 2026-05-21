#include "editor_transform_keyframe_ops.h"

#include "editor_shared_keyframes.h"

#include <QtGlobal>

#include <algorithm>

int findTransformKeyframeIndex(const TimelineClip& clip, int64_t frame) {
    for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
        if (clip.transformKeyframes[i].frame == frame) {
            return i;
        }
    }
    return -1;
}

bool upsertStoredTransformKeyframe(TimelineClip& clip,
                                   const TimelineClip::TransformKeyframe& keyframe) {
    if (!clipHasVisuals(clip)) {
        return false;
    }

    TimelineClip::TransformKeyframe stored = keyframe;
    stored.frame = qBound<int64_t>(
        0, stored.frame, qMax<int64_t>(0, clip.durationFrames - 1));

    const int index = findTransformKeyframeIndex(clip, stored.frame);
    if (index >= 0) {
        clip.transformKeyframes[index] = stored;
    } else {
        clip.transformKeyframes.push_back(stored);
    }
    normalizeClipTransformKeyframes(clip);
    return true;
}

bool removeStoredTransformKeyframes(TimelineClip& clip, const QList<int64_t>& frames) {
    if (frames.isEmpty()) {
        return false;
    }

    const int originalSize = clip.transformKeyframes.size();
    clip.transformKeyframes.erase(
        std::remove_if(clip.transformKeyframes.begin(),
                       clip.transformKeyframes.end(),
                       [&frames](const TimelineClip::TransformKeyframe& keyframe) {
                           return frames.contains(keyframe.frame);
                       }),
        clip.transformKeyframes.end());
    if (clip.transformKeyframes.size() == originalSize) {
        return false;
    }
    normalizeClipTransformKeyframes(clip);
    return true;
}

QSet<int64_t> duplicateStoredTransformKeyframesByDelta(TimelineClip& clip,
                                                       const QList<int64_t>& sourceFrames,
                                                       int64_t frameDelta) {
    QSet<int64_t> newFrames;
    if (sourceFrames.isEmpty()) {
        return newFrames;
    }

    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    const QVector<TimelineClip::TransformKeyframe> originalKeyframes = clip.transformKeyframes;
    for (const TimelineClip::TransformKeyframe& keyframe : originalKeyframes) {
        if (!sourceFrames.contains(keyframe.frame)) {
            continue;
        }
        TimelineClip::TransformKeyframe duplicate = keyframe;
        duplicate.frame = qBound<int64_t>(0, keyframe.frame + frameDelta, maxFrame);
        upsertStoredTransformKeyframe(clip, duplicate);
        newFrames.insert(duplicate.frame);
    }
    return newFrames;
}

bool duplicateStoredTransformKeyframesToFrame(TimelineClip& clip,
                                              const QList<int64_t>& sourceFrames,
                                              int64_t targetFrame) {
    if (sourceFrames.isEmpty()) {
        return false;
    }

    const int index = findTransformKeyframeIndex(clip, sourceFrames.constFirst());
    if (index < 0) {
        return false;
    }

    TimelineClip::TransformKeyframe duplicate = clip.transformKeyframes[index];
    duplicate.frame = qBound<int64_t>(0, targetFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    return upsertStoredTransformKeyframe(clip, duplicate);
}

TimelineClip::TransformKeyframe interpolateStoredTransformKeyframe(
    const TimelineClip::TransformKeyframe& earlier,
    const TimelineClip::TransformKeyframe& later,
    int64_t targetFrame) {
    const int64_t span = later.frame - earlier.frame;
    const qreal t = span > 0
                        ? static_cast<qreal>(targetFrame - earlier.frame) /
                              static_cast<qreal>(span)
                        : 0.0;

    TimelineClip::TransformKeyframe midpoint;
    midpoint.frame = targetFrame;
    midpoint.translationX = earlier.translationX + ((later.translationX - earlier.translationX) * t);
    midpoint.translationY = earlier.translationY + ((later.translationY - earlier.translationY) * t);
    midpoint.rotation = earlier.rotation + ((later.rotation - earlier.rotation) * t);
    midpoint.scaleX = earlier.scaleX + ((later.scaleX - earlier.scaleX) * t);
    midpoint.scaleY = earlier.scaleY + ((later.scaleY - earlier.scaleY) * t);
    midpoint.linearInterpolation = later.linearInterpolation;
    return midpoint;
}
