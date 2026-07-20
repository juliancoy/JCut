#include "editor_preview_edit_helpers.h"

#include "editor_shared_keyframes.h"
#include "editor_transform_keyframe_ops.h"
#include "titles.h"

#include <QDebug>
#include <QtGlobal>

namespace {
TimelineClip::TransformKeyframe identityVisualTransformKeyframe() {
    TimelineClip::TransformKeyframe keyframe;
    keyframe.frame = 0;
    keyframe.translationX = 0.0;
    keyframe.translationY = 0.0;
    keyframe.rotation = 0.0;
    keyframe.scaleX = 1.0;
    keyframe.scaleY = 1.0;
    keyframe.linearInterpolation = true;
    keyframe.maskRepeatDeltaX = 0.0;
    keyframe.maskRepeatDeltaY = 0.0;
    return keyframe;
}

bool applyStaticPreviewVisualTransform(TimelineClip& clip,
                                       qreal translationX,
                                       qreal translationY,
                                       qreal scaleX,
                                       qreal scaleY,
                                       bool updateScale) {
    if (!clipHasVisuals(clip)) {
        return false;
    }

    clip.baseTranslationX = translationX;
    clip.baseTranslationY = translationY;
    if (updateScale) {
        clip.baseScaleX = sanitizeScaleValue(scaleX);
        clip.baseScaleY = sanitizeScaleValue(scaleY);
    }

    clip.transformKeyframes.clear();
    clip.transformKeyframes.push_back(identityVisualTransformKeyframe());
    normalizeClipTransformKeyframes(clip);
    return true;
}

bool upsertPreviewVisualTransformKeyframe(TimelineClip& clip,
                                          int64_t keyframeTimelineFrame,
                                          qreal translationX,
                                          qreal translationY,
                                          qreal scaleX,
                                          qreal scaleY) {
    if (!clipHasVisuals(clip)) {
        return false;
    }

    const int64_t localFrame = qBound<int64_t>(
        0,
        keyframeTimelineFrame - clip.startFrame,
        qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame > 0) {
        bool hasFrameZero = false;
        for (const TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
            if (existing.frame == 0) {
                hasFrameZero = true;
                break;
            }
        }
        if (!hasFrameZero) {
            clip.transformKeyframes.push_back(identityVisualTransformKeyframe());
        }
    }

    TimelineClip::TransformKeyframe keyframe =
        evaluateClipKeyframeOffsetAtFrame(clip, keyframeTimelineFrame);
    keyframe.frame = localFrame;
    keyframe.translationX = translationX - clip.baseTranslationX;
    keyframe.translationY = translationY - clip.baseTranslationY;
    keyframe.scaleX =
        sanitizeScaleValue(scaleX / sanitizeScaleValue(clip.baseScaleX));
    keyframe.scaleY =
        sanitizeScaleValue(scaleY / sanitizeScaleValue(clip.baseScaleY));

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
        keyframe.windowWidth = evaluated.windowWidth;
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

bool stagePreviewMove(TimelineClip& clip,
                      int64_t keyframeTimelineFrame,
                      qreal translationX,
                      qreal translationY) {
    if (clip.mediaType == ClipMediaType::Title) {
        return stagePreviewTitleMoveKeyframe(
            clip, keyframeTimelineFrame, translationX, translationY);
    }
    Q_UNUSED(keyframeTimelineFrame);
    return applyStaticPreviewVisualTransform(
        clip,
        translationX,
        translationY,
        clip.baseScaleX,
        clip.baseScaleY,
        false);
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
    const bool updated = applyStaticPreviewVisualTransform(
        clip,
        translationX,
        translationY,
        clip.baseScaleX,
        clip.baseScaleY,
        false);
    qInfo().noquote()
        << QStringLiteral("[preview-move-commit] ok=%1 clip=%2 mode=static_base frame=%3 tx=%4 ty=%5")
               .arg(updated ? QStringLiteral("true") : QStringLiteral("false"),
                    clip.id,
                    QString::number(keyframeTimelineFrame),
                    QString::number(translationX, 'f', 3),
                    QString::number(translationY, 'f', 3));
    return updated;
}

bool commitPreviewTransform(TimelineClip& clip,
                            int64_t keyframeTimelineFrame,
                            qreal translationX,
                            qreal translationY,
                            qreal scaleX,
                            qreal scaleY,
                            bool transcriptOverlaySelected) {
    if (transcriptOverlaySelected &&
        clipSupportsTranscriptOverlayPreviewEdits(clip)) {
        clip.transcriptOverlay.translationX = translationX;
        clip.transcriptOverlay.translationY = translationY;
        clip.transcriptOverlay.useManualPlacement = true;
        clip.transcriptOverlay.boxWidth = qMax<qreal>(
            TimelineClip::TranscriptOverlaySettings::kMinReadableBoxWidth,
            scaleX);
        clip.transcriptOverlay.boxHeight = qMax<qreal>(
            TimelineClip::TranscriptOverlaySettings::kMinReadableBoxHeight,
            scaleY);
        return true;
    }

    const bool updated = upsertPreviewVisualTransformKeyframe(
        clip,
        keyframeTimelineFrame,
        translationX,
        translationY,
        scaleX,
        scaleY);
    qInfo().noquote()
        << QStringLiteral("[preview-transform-commit] ok=%1 clip=%2 mode=temporal_keyframe frame=%3 tx=%4 ty=%5 sx=%6 sy=%7")
               .arg(updated ? QStringLiteral("true") : QStringLiteral("false"),
                    clip.id,
                    QString::number(keyframeTimelineFrame),
                    QString::number(translationX, 'f', 3),
                    QString::number(translationY, 'f', 3),
                    QString::number(scaleX, 'f', 4),
                    QString::number(scaleY, 'f', 4));
    return updated;
}
