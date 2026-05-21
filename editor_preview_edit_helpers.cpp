#include "editor_preview_edit_helpers.h"

#include "editor_shared_keyframes.h"
#include "editor_transform_keyframe_ops.h"
#include "titles.h"

#include <QtGlobal>

#include <functional>

namespace {
bool upsertVisualTransformKeyframe(
    TimelineClip& clip,
    int64_t keyframeTimelineFrame,
    const std::function<void(TimelineClip::TransformKeyframe&)>& mutate) {
    if (!clipHasVisuals(clip)) {
        return false;
    }
    const TimelineClip::TransformKeyframe offset =
        evaluateClipKeyframeOffsetAtFrame(clip, keyframeTimelineFrame);
    const int64_t keyframeFrame =
        qBound<int64_t>(0,
                        keyframeTimelineFrame - clip.startFrame,
                        qMax<int64_t>(0, clip.durationFrames - 1));
    TimelineClip::TransformKeyframe keyframe = offset;
    keyframe.frame = keyframeFrame;
    mutate(keyframe);

    return upsertStoredTransformKeyframe(clip, keyframe);
}

bool stagePreviewTitleMoveKeyframe(TimelineClip& clip,
                                   int64_t keyframeTimelineFrame,
                                   qreal translationX,
                                   qreal translationY) {
    if (clip.mediaType != ClipMediaType::Title) {
        return false;
    }
    const int64_t localFrame =
        qBound<int64_t>(0,
                        keyframeTimelineFrame - clip.startFrame,
                        qMax<int64_t>(0, clip.durationFrames - 1));
    bool replaced = false;
    for (TimelineClip::TitleKeyframe& kf : clip.titleKeyframes) {
        if (kf.frame == localFrame) {
            kf.translationX = translationX;
            kf.translationY = translationY;
            replaced = true;
            break;
        }
    }
    if (!replaced && !clip.titleKeyframes.isEmpty()) {
        TimelineClip::TitleKeyframe keyframe = clip.titleKeyframes.constFirst();
        keyframe.frame = localFrame;
        keyframe.translationX = translationX;
        keyframe.translationY = translationY;
        clip.titleKeyframes.push_back(keyframe);
    }
    normalizeClipTitleKeyframes(clip);
    return true;
}

bool commitPreviewTitleMoveKeyframe(TimelineClip& clip,
                                    int64_t keyframeTimelineFrame,
                                    qreal translationX,
                                    qreal translationY) {
    if (clip.mediaType != ClipMediaType::Title) {
        return false;
    }
    const int64_t localFrame =
        qBound<int64_t>(0,
                        keyframeTimelineFrame - clip.startFrame,
                        qMax<int64_t>(0, clip.durationFrames - 1));
    bool replaced = false;
    for (TimelineClip::TitleKeyframe& kf : clip.titleKeyframes) {
        if (kf.frame == localFrame) {
            kf.translationX = translationX;
            kf.translationY = translationY;
            replaced = true;
            break;
        }
    }
    if (!replaced && !clip.titleKeyframes.isEmpty()) {
        int bestIdx = 0;
        for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
            if (clip.titleKeyframes[i].frame <= localFrame) {
                bestIdx = i;
            }
        }
        clip.titleKeyframes[bestIdx].translationX = translationX;
        clip.titleKeyframes[bestIdx].translationY = translationY;
    }
    normalizeClipTitleKeyframes(clip);
    return true;
}
}  // namespace

bool clipSupportsTranscriptOverlayPreviewEdits(const TimelineClip& clip) {
    return (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) &&
           clip.transcriptOverlay.enabled;
}

int64_t resolvePreviewDragKeyframeTimelineFrame(QHash<QString, int64_t>& anchorFrames,
                                                const QString& clipId,
                                                int64_t currentFrame,
                                                bool playing,
                                                bool finalize) {
    int64_t keyframeTimelineFrame = currentFrame;
    if (playing) {
        if (finalize) {
            keyframeTimelineFrame = anchorFrames.value(clipId, currentFrame);
            anchorFrames.remove(clipId);
        } else {
            keyframeTimelineFrame = anchorFrames.value(clipId, currentFrame);
            anchorFrames.insert(clipId, keyframeTimelineFrame);
        }
    } else if (finalize) {
        anchorFrames.remove(clipId);
    }
    return keyframeTimelineFrame;
}

bool createPreviewKeyframeAtTimelineFrame(TimelineClip& clip, int64_t timelineFrame) {
    const int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - clip.startFrame,
        qMax<int64_t>(0, clip.durationFrames - 1));

    if (clip.mediaType == ClipMediaType::Title) {
        if (clip.titleKeyframes.isEmpty()) {
            return false;
        }

        const EvaluatedTitle evaluated = evaluateTitleAtLocalFrame(clip, localFrame);
        if (!evaluated.valid) {
            return false;
        }

        TimelineClip::TitleKeyframe keyframe = clip.titleKeyframes.constFirst();
        keyframe.frame = localFrame;
        keyframe.text = evaluated.text;
        keyframe.translationX = evaluated.x;
        keyframe.translationY = evaluated.y;
        keyframe.fontSize = evaluated.fontSize;
        keyframe.opacity = evaluated.opacity;
        keyframe.fontFamily = evaluated.fontFamily;
        keyframe.bold = evaluated.bold;
        keyframe.italic = evaluated.italic;
        keyframe.color = evaluated.color;
        keyframe.dropShadowEnabled = evaluated.dropShadowEnabled;
        keyframe.dropShadowColor = evaluated.dropShadowColor;
        keyframe.dropShadowOpacity = evaluated.dropShadowOpacity;
        keyframe.dropShadowOffsetX = evaluated.dropShadowOffsetX;
        keyframe.dropShadowOffsetY = evaluated.dropShadowOffsetY;
        keyframe.windowEnabled = evaluated.windowEnabled;
        keyframe.windowColor = evaluated.windowColor;
        keyframe.windowOpacity = evaluated.windowOpacity;
        keyframe.windowPadding = evaluated.windowPadding;
        keyframe.windowFrameEnabled = evaluated.windowFrameEnabled;
        keyframe.windowFrameColor = evaluated.windowFrameColor;
        keyframe.windowFrameOpacity = evaluated.windowFrameOpacity;
        keyframe.windowFrameWidth = evaluated.windowFrameWidth;
        keyframe.windowFrameGap = evaluated.windowFrameGap;
        for (const TimelineClip::TitleKeyframe& existing : clip.titleKeyframes) {
            if (existing.frame > localFrame) {
                keyframe.linearInterpolation = existing.linearInterpolation;
                break;
            }
        }

        bool replaced = false;
        for (TimelineClip::TitleKeyframe& existing : clip.titleKeyframes) {
            if (existing.frame == localFrame) {
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.titleKeyframes.push_back(keyframe);
        }
        normalizeClipTitleKeyframes(clip);
        return true;
    }

    if (!clipHasVisuals(clip)) {
        return false;
    }

    const TimelineClip::TransformKeyframe evaluated =
        evaluateClipTransformAtFrame(clip, timelineFrame);
    TimelineClip::TransformKeyframe keyframe;
    keyframe.frame = localFrame;
    keyframe.translationX = evaluated.translationX - clip.baseTranslationX;
    keyframe.translationY = evaluated.translationY - clip.baseTranslationY;
    keyframe.rotation = evaluated.rotation - clip.baseRotation;
    keyframe.scaleX =
        sanitizeScaleValue(evaluated.scaleX / sanitizeScaleValue(clip.baseScaleX));
    keyframe.scaleY =
        sanitizeScaleValue(evaluated.scaleY / sanitizeScaleValue(clip.baseScaleY));
    for (const TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
        if (existing.frame > localFrame) {
            keyframe.linearInterpolation = existing.linearInterpolation;
            break;
        }
    }

    return upsertStoredTransformKeyframe(clip, keyframe);
}

bool stagePreviewResize(TimelineClip& clip,
                        int64_t keyframeTimelineFrame,
                        qreal scaleX,
                        qreal scaleY) {
    return upsertVisualTransformKeyframe(
        clip,
        keyframeTimelineFrame,
        [&](TimelineClip::TransformKeyframe& keyframe) {
            keyframe.scaleX =
                sanitizeScaleValue(scaleX / sanitizeScaleValue(clip.baseScaleX));
            keyframe.scaleY =
                sanitizeScaleValue(scaleY / sanitizeScaleValue(clip.baseScaleY));
        });
}

bool commitPreviewResize(TimelineClip& clip,
                         int64_t keyframeTimelineFrame,
                         qreal scaleX,
                         qreal scaleY,
                         bool transcriptOverlaySelected) {
    if (transcriptOverlaySelected &&
        clipSupportsTranscriptOverlayPreviewEdits(clip)) {
        clip.transcriptOverlay.boxWidth = qMax<qreal>(80.0, scaleX);
        clip.transcriptOverlay.boxHeight = qMax<qreal>(40.0, scaleY);
        return true;
    }
    return stagePreviewResize(clip, keyframeTimelineFrame, scaleX, scaleY);
}

bool stagePreviewMove(TimelineClip& clip,
                      int64_t keyframeTimelineFrame,
                      qreal translationX,
                      qreal translationY) {
    if (clip.mediaType == ClipMediaType::Title) {
        return stagePreviewTitleMoveKeyframe(
            clip, keyframeTimelineFrame, translationX, translationY);
    }
    return upsertVisualTransformKeyframe(
        clip,
        keyframeTimelineFrame,
        [&](TimelineClip::TransformKeyframe& keyframe) {
            keyframe.translationX = translationX - clip.baseTranslationX;
            keyframe.translationY = translationY - clip.baseTranslationY;
        });
}

bool commitPreviewMove(TimelineClip& clip,
                       int64_t keyframeTimelineFrame,
                       qreal translationX,
                       qreal translationY,
                       bool transcriptOverlaySelected) {
    if (transcriptOverlaySelected &&
        clipSupportsTranscriptOverlayPreviewEdits(clip)) {
        clip.transcriptOverlay.translationX = translationX;
        clip.transcriptOverlay.translationY = translationY;
        clip.transcriptOverlay.useManualPlacement = true;
        return true;
    }
    if (clip.mediaType == ClipMediaType::Title) {
        return commitPreviewTitleMoveKeyframe(
            clip, keyframeTimelineFrame, translationX, translationY);
    }
    return upsertVisualTransformKeyframe(
        clip,
        keyframeTimelineFrame,
        [&](TimelineClip::TransformKeyframe& keyframe) {
            keyframe.translationX = translationX - clip.baseTranslationX;
            keyframe.translationY = translationY - clip.baseTranslationY;
        });
}
