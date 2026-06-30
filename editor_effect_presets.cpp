#include "editor_effect_presets.h"

#include "timeline_fps.h"

bool applyNewsLowerThirdFlyInPreset(TimelineClip& clip)
{
    if (clip.mediaType != ClipMediaType::Title) {
        return false;
    }

    const int64_t duration = qMax<int64_t>(static_cast<int64_t>(kTimelineFps * 3), clip.durationFrames);
    const int64_t inEnd = qMin<int64_t>(duration - 1, qRound64(kTimelineFps * 0.35));
    const int64_t holdEnd =
        qMin<int64_t>(duration - 1, qMax<int64_t>(inEnd + 1, duration - qRound64(kTimelineFps * 0.45)));
    const int64_t outEnd = duration - 1;

    TimelineClip::TitleKeyframe base =
        clip.titleKeyframes.isEmpty() ? TimelineClip::TitleKeyframe{} : clip.titleKeyframes.constFirst();
    if (base.text.trimmed().isEmpty()) {
        base.text = clip.label.trimmed().isEmpty() ? QStringLiteral("Speaker Name") : clip.label;
    }
    base.translationY = qBound<qreal>(-0.95, base.translationY == 0.0 ? 0.68 : base.translationY, 0.95);
    base.windowEnabled = true;
    base.windowOpacity = qMax<qreal>(base.windowOpacity, 0.55);
    base.windowPadding = qMax<qreal>(base.windowPadding, 22.0);
    base.dropShadowEnabled = true;
    base.linearInterpolation = true;

    TimelineClip::TitleKeyframe before = base;
    before.frame = 0;
    before.translationX = -1.28;
    before.opacity = 0.0;

    TimelineClip::TitleKeyframe arrived = base;
    arrived.frame = inEnd;
    arrived.translationX = -0.34;
    arrived.opacity = 1.0;

    TimelineClip::TitleKeyframe hold = arrived;
    hold.frame = holdEnd;

    TimelineClip::TitleKeyframe after = arrived;
    after.frame = outEnd;
    after.translationX = 1.28;
    after.opacity = 0.0;

    clip.titleKeyframes = {before, arrived, hold, after};
    return true;
}

