#include "titles.h"
#include "overlay_render_backend.h"

#include <QUuid>
#include <cmath>

EvaluatedTitle evaluateTitleAtLocalFrame(const TimelineClip& clip, int64_t localFrame)
{
    EvaluatedTitle result;
    if (clip.titleKeyframes.isEmpty()) {
        return result;
    }

    // Find the keyframe at or before localFrame (step interpolation for text,
    // linear interpolation for numeric properties when enabled).
    int beforeIdx = 0;
    int afterIdx = -1;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame <= localFrame) {
            beforeIdx = i;
        } else if (afterIdx < 0) {
            afterIdx = i;
        }
    }

    const auto& kf = clip.titleKeyframes[beforeIdx];
    result.text = kf.text;
    // Title clips use their own coordinate system in titleKeyframes directly.
    // moveRequested writes to titleKeyframes, not baseTranslation.
    result.x = kf.translationX;
    result.y = kf.translationY;
    result.fontSize = kf.fontSize;
    result.opacity = kf.opacity;
    result.fontFamily = kf.fontFamily;
    result.bold = kf.bold;
    result.italic = kf.italic;
    result.color = kf.color;
    result.dropShadowEnabled = kf.dropShadowEnabled;
    result.dropShadowColor = kf.dropShadowColor;
    result.dropShadowOpacity = kf.dropShadowOpacity;
    result.dropShadowOffsetX = kf.dropShadowOffsetX;
    result.dropShadowOffsetY = kf.dropShadowOffsetY;
    result.windowEnabled = kf.windowEnabled;
    result.windowColor = kf.windowColor;
    result.windowOpacity = kf.windowOpacity;
    result.windowPadding = kf.windowPadding;
    result.windowFrameEnabled = kf.windowFrameEnabled;
    result.windowFrameColor = kf.windowFrameColor;
    result.windowFrameOpacity = kf.windowFrameOpacity;
    result.windowFrameWidth = kf.windowFrameWidth;
    result.windowFrameGap = kf.windowFrameGap;
    result.valid = true;

    // Linear interpolation of numeric properties between keyframes.
    // Match the other keyframe evaluators: the segment interpolation mode
    // is owned by the destination (next) keyframe.
    if (afterIdx >= 0) {
        const auto& next = clip.titleKeyframes[afterIdx];
        if (!next.linearInterpolation) {
            return result;
        }
        const int64_t span = next.frame - kf.frame;
        if (span > 0) {
            const qreal t = static_cast<qreal>(localFrame - kf.frame) / static_cast<qreal>(span);
            result.x = kf.translationX + (next.translationX - kf.translationX) * t;
            result.y = kf.translationY + (next.translationY - kf.translationY) * t;
            result.fontSize = kf.fontSize + (next.fontSize - kf.fontSize) * t;
            result.opacity = kf.opacity + (next.opacity - kf.opacity) * t;
            // Text, font, bold, italic, color are NOT interpolated — they step at the keyframe
        }
    }

    return result;
}

EvaluatedTitle composeTitleWithOpacity(const EvaluatedTitle& title, qreal opacityMultiplier)
{
    EvaluatedTitle composed = title;
    if (!composed.valid) {
        return composed;
    }
    composed.opacity = qBound<qreal>(0.0, composed.opacity * opacityMultiplier, 1.0);
    return composed;
}

TitleLayoutMetrics measureTitleLayout(const EvaluatedTitle& title, qreal fontScale)
{
    return render_detail::measureOverlayTitleLayout(title, fontScale);
}

TimelineClip createDefaultTitleClip(int64_t startFrame, int trackIndex, int64_t durationFrames)
{
    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.mediaType = ClipMediaType::Title;
    clip.label = QStringLiteral("Title");
    clip.startFrame = startFrame;
    clip.trackIndex = trackIndex;
    clip.durationFrames = durationFrames;
    clip.sourceDurationFrames = durationFrames;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.color = QColor(QStringLiteral("#4a2d6b"));

    TimelineClip::TitleKeyframe defaultKeyframe;
    defaultKeyframe.frame = 0;
    defaultKeyframe.text = QStringLiteral("Title");
    defaultKeyframe.translationX = 0.0;
    defaultKeyframe.translationY = 0.0;
    defaultKeyframe.fontSize = 48.0;
    defaultKeyframe.opacity = 1.0;
    clip.titleKeyframes.push_back(defaultKeyframe);

    return clip;
}

render_detail::OverlayImage renderTitleOverlay(const QSize& imageSize,
                                               const EvaluatedTitle& title,
                                               const QSize& outputSize)
{
    return render_detail::overlayRenderBackend().renderTitleOverlay(imageSize, title, outputSize);
}
