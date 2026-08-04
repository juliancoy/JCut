#include "editor_title_opacity_keyframe_ops.h"

#include "editor_shared_keyframes.h"
#include "keyframe_sequence.h"

#include <QtGlobal>

#include <algorithm>

int findTitleKeyframeIndex(const TimelineClip& clip, int64_t frame) {
    return jcut::keyframes::findFrameIndex(clip.titleKeyframes, frame);
}

bool upsertStoredTitleKeyframe(TimelineClip& clip,
                               const TimelineClip::TitleKeyframe& keyframe) {
    TimelineClip::TitleKeyframe stored = keyframe;
    stored.frame = qBound<int64_t>(0, stored.frame, qMax<int64_t>(0, clip.durationFrames - 1));
    jcut::keyframes::upsertByFrame(&clip.titleKeyframes, stored);
    normalizeClipTitleKeyframes(clip);
    return true;
}

bool replaceStoredTitleKeyframeAtFrame(TimelineClip& clip,
                                       int64_t originalFrame,
                                       const TimelineClip::TitleKeyframe& keyframe) {
    TimelineClip::TitleKeyframe stored = keyframe;
    stored.frame = qBound<int64_t>(0, stored.frame, qMax<int64_t>(0, clip.durationFrames - 1));
    jcut::keyframes::replaceAtFrame(&clip.titleKeyframes, originalFrame, stored);
    normalizeClipTitleKeyframes(clip);
    return true;
}

bool updateStoredTitleKeyframeAtFrame(TimelineClip& clip,
                                      int64_t frame,
                                      const std::function<void(TimelineClip::TitleKeyframe&)>& mutate) {
    const int index = findTitleKeyframeIndex(clip, frame);
    if (index < 0) {
        return false;
    }
    mutate(clip.titleKeyframes[index]);
    normalizeClipTitleKeyframes(clip);
    return true;
}

bool removeStoredTitleKeyframes(TimelineClip& clip, const QSet<int64_t>& frames) {
    if (frames.isEmpty()) {
        return false;
    }
    if (!jcut::keyframes::removeIf(
            &clip.titleKeyframes,
            [&frames](const TimelineClip::TitleKeyframe& keyframe) {
                return frames.contains(keyframe.frame);
            })) {
        return false;
    }
    normalizeClipTitleKeyframes(clip);
    return true;
}

TimelineClip::TitleKeyframe interpolateStoredTitleKeyframe(
    const TimelineClip::TitleKeyframe& earlier,
    const TimelineClip::TitleKeyframe& later,
    int64_t targetFrame) {
    const int64_t span = later.frame - earlier.frame;
    const qreal t = span > 0
                        ? static_cast<qreal>(targetFrame - earlier.frame) /
                              static_cast<qreal>(span)
                        : 0.0;

    TimelineClip::TitleKeyframe midpoint;
    midpoint.frame = targetFrame;
    midpoint.text = earlier.text;
    midpoint.translationX = earlier.translationX + ((later.translationX - earlier.translationX) * t);
    midpoint.translationY = earlier.translationY + ((later.translationY - earlier.translationY) * t);
    midpoint.fontSize = earlier.fontSize + ((later.fontSize - earlier.fontSize) * t);
    midpoint.opacity = earlier.opacity + ((later.opacity - earlier.opacity) * t);
    midpoint.fontFamily = earlier.fontFamily;
    midpoint.bold = earlier.bold;
    midpoint.italic = earlier.italic;
    midpoint.color = earlier.color;
    midpoint.dropShadowEnabled = earlier.dropShadowEnabled;
    midpoint.dropShadowColor = earlier.dropShadowColor;
    midpoint.dropShadowOpacity = earlier.dropShadowOpacity;
    midpoint.dropShadowOffsetX = earlier.dropShadowOffsetX;
    midpoint.dropShadowOffsetY = earlier.dropShadowOffsetY;
    midpoint.windowEnabled = earlier.windowEnabled;
    midpoint.windowColor = earlier.windowColor;
    midpoint.windowOpacity = earlier.windowOpacity;
    midpoint.windowPadding = earlier.windowPadding;
    midpoint.windowWidth = earlier.windowWidth + ((later.windowWidth - earlier.windowWidth) * t);
    midpoint.windowFrameEnabled = earlier.windowFrameEnabled;
    midpoint.windowFrameColor = earlier.windowFrameColor;
    midpoint.windowFrameOpacity = earlier.windowFrameOpacity;
    midpoint.windowFrameWidth = earlier.windowFrameWidth;
    midpoint.windowFrameGap = earlier.windowFrameGap;
    midpoint.linearInterpolation = later.linearInterpolation;
    return midpoint;
}

int findOpacityKeyframeIndex(const TimelineClip& clip, int64_t frame) {
    return jcut::keyframes::findFrameIndex(clip.opacityKeyframes, frame);
}

bool upsertStoredOpacityKeyframe(TimelineClip& clip,
                                 const TimelineClip::OpacityKeyframe& keyframe) {
    TimelineClip::OpacityKeyframe stored = keyframe;
    stored.frame = qBound<int64_t>(0, stored.frame, qMax<int64_t>(0, clip.durationFrames - 1));
    jcut::keyframes::upsertByFrame(&clip.opacityKeyframes, stored);
    normalizeClipOpacityKeyframes(clip);
    return true;
}

bool replaceStoredOpacityKeyframeAtFrame(TimelineClip& clip,
                                         int64_t originalFrame,
                                         const TimelineClip::OpacityKeyframe& keyframe) {
    TimelineClip::OpacityKeyframe stored = keyframe;
    stored.frame = qBound<int64_t>(0, stored.frame, qMax<int64_t>(0, clip.durationFrames - 1));
    jcut::keyframes::replaceAtFrame(&clip.opacityKeyframes, originalFrame, stored);
    normalizeClipOpacityKeyframes(clip);
    return true;
}

bool upsertOpacityKeyframePreservingInterpolation(TimelineClip& clip,
                                                  int64_t frame,
                                                  qreal opacity) {
    TimelineClip::OpacityKeyframe keyframe;
    keyframe.frame = qBound<int64_t>(0, frame, qMax<int64_t>(0, clip.durationFrames - 1));
    keyframe.opacity = opacity;
    keyframe.linearInterpolation = true;

    const int existingIndex = findOpacityKeyframeIndex(clip, keyframe.frame);
    if (existingIndex >= 0) {
        keyframe.linearInterpolation = clip.opacityKeyframes[existingIndex].linearInterpolation;
        clip.opacityKeyframes[existingIndex] = keyframe;
    } else {
        for (const TimelineClip::OpacityKeyframe& existing : clip.opacityKeyframes) {
            if (existing.frame > keyframe.frame) {
                keyframe.linearInterpolation = existing.linearInterpolation;
                break;
            }
        }
        clip.opacityKeyframes.push_back(keyframe);
    }
    normalizeClipOpacityKeyframes(clip);
    return true;
}

bool removeStoredOpacityKeyframes(TimelineClip& clip,
                                  const QList<int64_t>& frames,
                                  bool keepFrameZero) {
    if (frames.isEmpty()) {
        return false;
    }
    if (!jcut::keyframes::removeIf(
            &clip.opacityKeyframes,
            [&frames, keepFrameZero](const TimelineClip::OpacityKeyframe& keyframe) {
                return (!keepFrameZero || keyframe.frame > 0) &&
                       frames.contains(keyframe.frame);
            })) {
        return false;
    }
    normalizeClipOpacityKeyframes(clip);
    return true;
}
