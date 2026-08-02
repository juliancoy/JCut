#include "titles.h"
#include "transform_skip_aware_timing.h"
#include "cpu_overlay_render_backend.h"

#include <QUuid>
#include <cmath>

namespace {

qreal smoothStep(qreal t)
{
    const qreal bounded = qBound<qreal>(0.0, t, 1.0);
    return bounded * bounded * (3.0 - 2.0 * bounded);
}

struct LifetimeMotionProgress {
    qreal offset = 1.0;
    qreal opacity = 1.0;
    qreal normalizedTime = 0.0;
};

LifetimeMotionProgress titleLifetimeMotionProgress(const TimelineClip& clip, qreal localFrame)
{
    LifetimeMotionProgress progress;
    const qreal duration = static_cast<qreal>(qMax<int64_t>(1, clip.durationFrames));
    progress.normalizedTime = qBound<qreal>(0.0, localFrame / qMax<qreal>(1.0, duration - 1.0), 1.0);
    const qreal requestedFlyFrames =
        qBound<qreal>(3.0,
                      clip.titleLifetimeEffectFlySeconds * static_cast<qreal>(kTimelineFps),
                      qMax<qreal>(3.0, duration * 0.45));
    const qreal exitStart = qMax<qreal>(0.0, duration - requestedFlyFrames);
    if (localFrame < requestedFlyFrames) {
        progress.offset = smoothStep(localFrame / requestedFlyFrames);
        progress.opacity = progress.offset;
    } else if (localFrame >= exitStart) {
        const qreal exitProgress = smoothStep((localFrame - exitStart) / requestedFlyFrames);
        progress.offset = 1.0 - exitProgress;
        progress.opacity = 1.0 - exitProgress;
    }
    return progress;
}

void applySportsPanelStyle(EvaluatedTitle* title,
                           const QColor& panelColor,
                           const QColor& accentColor,
                           qreal panelOpacity,
                           qreal frameWidth,
                           qreal padding)
{
    if (!title) {
        return;
    }
    title->windowEnabled = true;
    title->windowColor = panelColor;
    title->windowOpacity = qMax<qreal>(title->windowOpacity, panelOpacity);
    title->windowPadding = qMax<qreal>(title->windowPadding, padding);
    title->windowFrameEnabled = true;
    title->windowFrameColor = accentColor;
    title->windowFrameOpacity = qMax<qreal>(title->windowFrameOpacity, 0.96);
    title->windowFrameWidth = qMax<qreal>(title->windowFrameWidth, frameWidth);
    title->windowFrameGap = qMax<qreal>(title->windowFrameGap, 5.0);
    title->dropShadowEnabled = true;
    title->dropShadowColor = QColor(QStringLiteral("#000000"));
    title->dropShadowOpacity = qMax<qreal>(title->dropShadowOpacity, 0.80);
    title->dropShadowOffsetX = qMax<qreal>(title->dropShadowOffsetX, 8.0);
    title->dropShadowOffsetY = qMax<qreal>(title->dropShadowOffsetY, 9.0);
    title->vulkan3DEnabled = true;
    title->vulkan3DExtrudeEnabled = true;
    if (title->textExtrudeMode == TextExtrudeMode::None) {
        title->textExtrudeMode = TextExtrudeMode::StackedCopies;
    }
    title->vulkan3DExtrudeDepth = qMax<qreal>(title->vulkan3DExtrudeDepth, 0.12);
    title->vulkan3DBevelScale = qMax<qreal>(title->vulkan3DBevelScale, 0.62);
}

void applyTitleLifetimeEffect(const TimelineClip& clip,
                              qreal localFrame,
                              EvaluatedTitle* title)
{
    if (!title || !title->valid ||
        clip.titleLifetimeEffect == TitleLifetimeEffect::None ||
        clip.durationFrames <= 1) {
        return;
    }

    const LifetimeMotionProgress progress = titleLifetimeMotionProgress(clip, localFrame);

    const qreal side = clip.titleLifetimeEffect == TitleLifetimeEffect::NewsFlyInRight
        ? 1.0
        : -1.0;
    const qreal offscreenOffset = qMax<qreal>(520.0, title->windowWidth + title->fontSize * 12.0);
    const qreal pi = 3.14159265358979323846;

    switch (clip.titleLifetimeEffect) {
    case TitleLifetimeEffect::NewsFlyInLeft:
    case TitleLifetimeEffect::NewsFlyInRight:
        title->x += side * offscreenOffset * (1.0 - progress.offset);
        applySportsPanelStyle(title,
                              QColor(QStringLiteral("#101826")),
                              QColor(QStringLiteral("#38c7ff")),
                              0.72,
                              3.0,
                              18.0);
        title->dropShadowOffsetX = side * -8.0;
        title->vulkan3DYawDegrees += side * -18.0 * (1.0 - progress.offset);
        title->vulkan3DDepth += 0.12 * (1.0 - progress.offset);
        break;
    case TitleLifetimeEffect::SportsLowerThird:
        title->x -= 260.0 * (1.0 - progress.offset);
        title->y += 285.0;
        title->windowWidth = qMax<qreal>(title->windowWidth, 780.0);
        title->textMaterialStyle = TitleMaterialStyle::DiagonalStripes;
        applySportsPanelStyle(title,
                              QColor(QStringLiteral("#09111f")),
                              QColor(QStringLiteral("#ffb000")),
                              0.82,
                              4.0,
                              20.0);
        title->vulkan3DYawDegrees -= 10.0 * (1.0 - progress.offset);
        break;
    case TitleLifetimeEffect::SportsScorebug: {
        const qreal pop = 1.0 + 0.10 * std::sin(qMin<qreal>(1.0, progress.normalizedTime * 8.0) * pi);
        title->x -= 610.0;
        title->y -= 360.0 + 80.0 * (1.0 - progress.offset);
        title->fontSize = qMax<qreal>(24.0, title->fontSize * 0.72 * pop);
        title->windowWidth = qMax<qreal>(title->windowWidth, 360.0);
        title->textMaterialStyle = TitleMaterialStyle::Neon;
        applySportsPanelStyle(title,
                              QColor(QStringLiteral("#07101d")),
                              QColor(QStringLiteral("#16d6ff")),
                              0.88,
                              3.0,
                              14.0);
        title->vulkan3DExtrudeDepth = qMax<qreal>(title->vulkan3DExtrudeDepth, 0.10);
        break;
    }
    case TitleLifetimeEffect::SportsStatCard:
        title->x += 430.0 + 440.0 * (1.0 - progress.offset);
        title->y += 45.0;
        title->windowWidth = qMax<qreal>(title->windowWidth, 520.0);
        title->textMaterialStyle = TitleMaterialStyle::Grid;
        title->windowFrameMaterialStyle = TitleMaterialStyle::Neon;
        applySportsPanelStyle(title,
                              QColor(QStringLiteral("#081421")),
                              QColor(QStringLiteral("#42ff9e")),
                              0.86,
                              4.0,
                              22.0);
        title->vulkan3DYawDegrees += 16.0 * (1.0 - progress.offset);
        title->vulkan3DDepth += 0.18 * (1.0 - progress.offset);
        break;
    case TitleLifetimeEffect::SportsMatchupBanner: {
        const qreal settle = 1.0 + 0.08 * std::sin(qMin<qreal>(1.0, progress.normalizedTime * 6.0) * pi);
        title->y += 330.0 + 130.0 * (1.0 - progress.offset);
        title->fontSize *= settle;
        title->windowWidth = qMax<qreal>(title->windowWidth, 980.0);
        title->textMaterialStyle = TitleMaterialStyle::DiagonalStripes;
        applySportsPanelStyle(title,
                              QColor(QStringLiteral("#111827")),
                              QColor(QStringLiteral("#f43f5e")),
                              0.84,
                              5.0,
                              24.0);
        title->vulkan3DPitchDegrees -= 7.0 * (1.0 - progress.offset);
        break;
    }
    case TitleLifetimeEffect::SportsReplayTag: {
        const qreal pulse = 1.0 + 0.05 * std::sin(progress.normalizedTime * pi * 6.0);
        title->x += 485.0 + 180.0 * (1.0 - progress.offset);
        title->y -= 310.0;
        title->fontSize *= pulse;
        title->windowWidth = qMax<qreal>(title->windowWidth, 300.0);
        title->textMaterialStyle = TitleMaterialStyle::Neon;
        applySportsPanelStyle(title,
                              QColor(QStringLiteral("#20110a")),
                              QColor(QStringLiteral("#ff7a00")),
                              0.82,
                              4.0,
                              15.0);
        title->vulkan3DYawDegrees += 24.0 * (1.0 - progress.offset);
        title->vulkan3DRollDegrees -= 8.0;
        title->vulkan3DScale = qMax<qreal>(0.01, title->vulkan3DScale * pulse);
        break;
    }
    case TitleLifetimeEffect::None:
        return;
    }

    title->opacity = qBound<qreal>(0.0, title->opacity * progress.opacity, 1.0);
}

}  // namespace

EvaluatedTitle evaluateTitleAtLocalFrame(const TimelineClip& clip, qreal localFrame)
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
    result.autoFitToOutput = kf.autoFitToOutput;
    result.opacity = kf.opacity;
    result.fontFamily = kf.fontFamily;
    result.bold = kf.bold;
    result.italic = kf.italic;
    result.color = kf.color;
    result.logoPath = kf.logoPath;
    result.textMaterialStyle = kf.textMaterialStyle;
    result.textPatternImagePath = kf.textPatternImagePath;
    result.textPatternScale = kf.textPatternScale;
    result.dropShadowEnabled = kf.dropShadowEnabled;
    result.dropShadowColor = kf.dropShadowColor;
    result.dropShadowOpacity = kf.dropShadowOpacity;
    result.dropShadowOffsetX = kf.dropShadowOffsetX;
    result.dropShadowOffsetY = kf.dropShadowOffsetY;
    result.windowEnabled = kf.windowEnabled;
    result.windowColor = kf.windowColor;
    result.windowOpacity = kf.windowOpacity;
    result.windowPadding = kf.windowPadding;
    result.windowWidth = kf.windowWidth;
    result.windowFrameEnabled = kf.windowFrameEnabled;
    result.windowFrameColor = kf.windowFrameColor;
    result.windowFrameOpacity = kf.windowFrameOpacity;
    result.windowFrameWidth = kf.windowFrameWidth;
    result.windowFrameGap = kf.windowFrameGap;
    result.windowFrameMaterialStyle = kf.windowFrameMaterialStyle;
    result.windowFramePatternImagePath = kf.windowFramePatternImagePath;
    result.windowFramePatternScale = kf.windowFramePatternScale;
    result.vulkan3DEnabled = kf.vulkan3DEnabled;
    result.vulkan3DExtrudeEnabled = kf.vulkan3DExtrudeEnabled;
    result.textExtrudeMode = kf.textExtrudeMode;
    result.vulkan3DExtrudeDepth = kf.vulkan3DExtrudeDepth;
    result.vulkan3DBevelScale = kf.vulkan3DBevelScale;
    result.vulkan3DYawDegrees = kf.vulkan3DYawDegrees;
    result.vulkan3DPitchDegrees = kf.vulkan3DPitchDegrees;
    result.vulkan3DRollDegrees = kf.vulkan3DRollDegrees;
    result.vulkan3DDepth = kf.vulkan3DDepth;
    result.vulkan3DScale = kf.vulkan3DScale;
    result.valid = true;

    // Linear interpolation of numeric properties between keyframes.
    // Match the other keyframe evaluators: the segment interpolation mode
    // is owned by the destination (next) keyframe.
    if (afterIdx >= 0) {
        const auto& next = clip.titleKeyframes[afterIdx];
        const int64_t span = next.frame - kf.frame;
        if (next.linearInterpolation && span > 0) {
            const qreal t = static_cast<qreal>(localFrame - kf.frame) / static_cast<qreal>(span);
            result.x = kf.translationX + (next.translationX - kf.translationX) * t;
            result.y = kf.translationY + (next.translationY - kf.translationY) * t;
            result.fontSize = kf.fontSize + (next.fontSize - kf.fontSize) * t;
            result.opacity = kf.opacity + (next.opacity - kf.opacity) * t;
            result.windowWidth = kf.windowWidth + (next.windowWidth - kf.windowWidth) * t;
            result.vulkan3DEnabled = kf.vulkan3DEnabled || next.vulkan3DEnabled;
            result.vulkan3DExtrudeEnabled = kf.vulkan3DExtrudeEnabled || next.vulkan3DExtrudeEnabled;
            result.textExtrudeMode = kf.textExtrudeMode != TextExtrudeMode::None
                ? kf.textExtrudeMode
                : next.textExtrudeMode;
            result.vulkan3DExtrudeDepth = kf.vulkan3DExtrudeDepth + (next.vulkan3DExtrudeDepth - kf.vulkan3DExtrudeDepth) * t;
            result.vulkan3DBevelScale = kf.vulkan3DBevelScale + (next.vulkan3DBevelScale - kf.vulkan3DBevelScale) * t;
            result.vulkan3DYawDegrees = kf.vulkan3DYawDegrees + (next.vulkan3DYawDegrees - kf.vulkan3DYawDegrees) * t;
            result.vulkan3DPitchDegrees = kf.vulkan3DPitchDegrees + (next.vulkan3DPitchDegrees - kf.vulkan3DPitchDegrees) * t;
            result.vulkan3DRollDegrees = kf.vulkan3DRollDegrees + (next.vulkan3DRollDegrees - kf.vulkan3DRollDegrees) * t;
            result.vulkan3DDepth = kf.vulkan3DDepth + (next.vulkan3DDepth - kf.vulkan3DDepth) * t;
            result.vulkan3DScale = kf.vulkan3DScale + (next.vulkan3DScale - kf.vulkan3DScale) * t;
            result.textPatternScale = kf.textPatternScale + (next.textPatternScale - kf.textPatternScale) * t;
            result.windowFramePatternScale = kf.windowFramePatternScale + (next.windowFramePatternScale - kf.windowFramePatternScale) * t;
            // Text, font, bold, italic, color are NOT interpolated — they step at the keyframe
        }
    }

    applyTitleLifetimeEffect(clip, localFrame, &result);
    return result;
}

EvaluatedTitle evaluateTitleAtTimelinePosition(const TimelineClip& clip,
                                               qreal timelineFramePosition,
                                               const PlaybackTimingContext& timing)
{
    const qreal animationFrame =
        clip.effectSkipAwareTiming
            ? clipPlaybackFramePositionForTimelineFrame(
                  clip, timelineFramePosition, timing)
            : qBound<qreal>(
                  0.0,
                  timelineFramePosition - static_cast<qreal>(clip.startFrame),
                  static_cast<qreal>(qMax<int64_t>(
                      0, clip.durationFrames - 1)));
    return evaluateTitleAtLocalFrame(clip, animationFrame);
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

EvaluatedTitle prepareRenderableTitleForVulkanText(const TimelineClip& clip,
                                                   qreal timelineFramePosition,
                                                   const PlaybackTimingContext& timing,
                                                   qreal opacityMultiplier,
                                                   const QSize& outputSize)
{
    const EvaluatedTitle evaluated =
        evaluateTitleAtTimelinePosition(clip, timelineFramePosition, timing);
    EvaluatedTitle prepared = fitTitleToOutput(
        composeTitleWithOpacity(evaluated, opacityMultiplier),
        outputSize);
    if (!prepared.valid ||
        prepared.text.trimmed().isEmpty() ||
        prepared.opacity <= 0.001) {
        prepared.valid = false;
    }
    return prepared;
}

TitleLayoutMetrics measureTitleLayout(const EvaluatedTitle& title, qreal fontScale)
{
    return render_detail::measureOverlayTitleLayout(title, fontScale);
}

EvaluatedTitle fitTitleToOutput(const EvaluatedTitle& title,
                                const QSize& outputSize,
                                qreal safeWidthFraction,
                                qreal safeHeightFraction)
{
    if (!title.valid || !title.autoFitToOutput || !outputSize.isValid()) return title;
    EvaluatedTitle fitted = title;
    const qreal safeWidth = outputSize.width() * qBound<qreal>(0.25, safeWidthFraction, 1.0);
    const qreal safeHeight = outputSize.height() * qBound<qreal>(0.25, safeHeightFraction, 1.0);
    const qreal padding = fitted.windowEnabled || fitted.windowFrameEnabled
        ? qMax<qreal>(0.0, fitted.windowPadding) * 2.0 : 0.0;
    const qreal extrusionSafety = fitted.vulkan3DEnabled ? 0.88 : 1.0;
    const TitleLayoutMetrics metrics = measureTitleLayout(fitted);
    const qreal widthScale = metrics.width > 0.0
        ? qMax<qreal>(0.01, (safeWidth - padding) / metrics.width) : 1.0;
    const qreal heightScale = metrics.height > 0.0
        ? qMax<qreal>(0.01, (safeHeight - padding) / metrics.height) : 1.0;
    const qreal scale = qMin<qreal>(1.0, qMin(widthScale, heightScale) * extrusionSafety);
    fitted.fontSize = qMax<qreal>(1.0, fitted.fontSize * scale);
    const TitleLayoutMetrics fittedMetrics = measureTitleLayout(fitted);
    const qreal correction = qMin(
        fittedMetrics.width > 0.0 ? (safeWidth - padding) / fittedMetrics.width : 1.0,
        fittedMetrics.height > 0.0 ? (safeHeight - padding) / fittedMetrics.height : 1.0);
    if (correction < 1.0) {
        fitted.fontSize = qMax<qreal>(1.0, fitted.fontSize * correction * 0.98);
    }
    if (fitted.windowWidth > 0.0) {
        fitted.windowWidth = qMin(fitted.windowWidth, safeWidth);
    }
    return fitted;
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
