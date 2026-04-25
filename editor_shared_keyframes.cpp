#include "editor_shared.h"
#include "transform_skip_aware_timing.h"

#include <QSet>
#include <QtGlobal>

#include <algorithm>
#include <cmath>


qreal sanitizeScaleValue(qreal value) {
    if (std::abs(value) < 0.01) {
        return value < 0.0 ? -0.01 : 0.01;
    }
    return value;
}

void normalizeClipTransformKeyframes(TimelineClip& clip) {
    clip.baseScaleX = sanitizeScaleValue(clip.baseScaleX);
    clip.baseScaleY = sanitizeScaleValue(clip.baseScaleY);
    std::sort(clip.transformKeyframes.begin(), clip.transformKeyframes.end(),
              [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::TransformKeyframe> normalized;
    normalized.reserve(clip.transformKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::TransformKeyframe keyframe : clip.transformKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.scaleX = sanitizeScaleValue(keyframe.scaleX);
        keyframe.scaleY = sanitizeScaleValue(keyframe.scaleY);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }
    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = 0;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            TimelineClip::TransformKeyframe firstKeyframe = normalized.constFirst();
            firstKeyframe.frame = 0;
            normalized.push_front(firstKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }
    clip.transformKeyframes = normalized;

    std::sort(clip.speakerFramingKeyframes.begin(), clip.speakerFramingKeyframes.end(),
              [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                  return a.frame < b.frame;
              });
    QVector<TimelineClip::TransformKeyframe> normalizedSpeakerFraming;
    normalizedSpeakerFraming.reserve(clip.speakerFramingKeyframes.size());
    for (TimelineClip::TransformKeyframe keyframe : clip.speakerFramingKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.scaleX = sanitizeScaleValue(keyframe.scaleX);
        keyframe.scaleY = sanitizeScaleValue(keyframe.scaleY);
        keyframe.rotation = 0.0;
        if (!normalizedSpeakerFraming.isEmpty() &&
            normalizedSpeakerFraming.constLast().frame == keyframe.frame) {
            normalizedSpeakerFraming.last() = keyframe;
        } else {
            normalizedSpeakerFraming.push_back(keyframe);
        }
    }
    clip.speakerFramingKeyframes = normalizedSpeakerFraming;
    clip.speakerFramingTargetXNorm = qBound<qreal>(0.0, clip.speakerFramingTargetXNorm, 1.0);
    clip.speakerFramingTargetYNorm = qBound<qreal>(0.0, clip.speakerFramingTargetYNorm, 1.0);
    clip.speakerFramingTargetBoxNorm = qBound<qreal>(-1.0, clip.speakerFramingTargetBoxNorm, 1.0);
    clip.speakerFramingMinConfidence = qBound<qreal>(0.0, clip.speakerFramingMinConfidence, 1.0);
}

void normalizeClipGradingKeyframes(TimelineClip& clip) {
    std::sort(clip.gradingKeyframes.begin(), clip.gradingKeyframes.end(),
              [](const TimelineClip::GradingKeyframe& a, const TimelineClip::GradingKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::GradingKeyframe> normalized;
    normalized.reserve(clip.gradingKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::GradingKeyframe keyframe : clip.gradingKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }

    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::GradingKeyframe keyframe;
            keyframe.frame = 0;
            keyframe.brightness = clip.brightness;
            keyframe.contrast = clip.contrast;
            keyframe.saturation = clip.saturation;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            // FIX: Create a keyframe at frame 0 with clip's base values
            // instead of duplicating the first keyframe
            TimelineClip::GradingKeyframe baseKeyframe;
            baseKeyframe.frame = 0;
            baseKeyframe.brightness = clip.brightness;
            baseKeyframe.contrast = clip.contrast;
            baseKeyframe.saturation = clip.saturation;
            // Use default values for shadows/midtones/highlights (0.0)
            // This allows proper interpolation from frame 0 to first keyframe
            normalized.push_front(baseKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }

    // Legacy cleanup: older builds stored opacity edits as grading keyframes too.
    // Drop grading keys that sit on opacity-key frames and do not change grading
    // relative to linear interpolation between surrounding grading keys.
    if (normalized.size() >= 3 && !clip.opacityKeyframes.isEmpty()) {
        QSet<int64_t> opacityFrames;
        opacityFrames.reserve(clip.opacityKeyframes.size());
        for (const TimelineClip::OpacityKeyframe& opacityKeyframe : clip.opacityKeyframes) {
            opacityFrames.insert(opacityKeyframe.frame);
        }

        auto nearlyEqual = [](qreal a, qreal b) {
            return std::abs(a - b) <= 0.000001;
        };

        auto gradingValuesMatch = [&nearlyEqual](const TimelineClip::GradingKeyframe& a,
                                                 const TimelineClip::GradingKeyframe& b) {
            return nearlyEqual(a.brightness, b.brightness) &&
                   nearlyEqual(a.contrast, b.contrast) &&
                   nearlyEqual(a.saturation, b.saturation) &&
                   nearlyEqual(a.shadowsR, b.shadowsR) &&
                   nearlyEqual(a.shadowsG, b.shadowsG) &&
                   nearlyEqual(a.shadowsB, b.shadowsB) &&
                   nearlyEqual(a.midtonesR, b.midtonesR) &&
                   nearlyEqual(a.midtonesG, b.midtonesG) &&
                   nearlyEqual(a.midtonesB, b.midtonesB) &&
                   nearlyEqual(a.highlightsR, b.highlightsR) &&
                   nearlyEqual(a.highlightsG, b.highlightsG) &&
                   nearlyEqual(a.highlightsB, b.highlightsB);
        };

        auto blended = [](const TimelineClip::GradingKeyframe& previous,
                          const TimelineClip::GradingKeyframe& next,
                          qreal t) {
            TimelineClip::GradingKeyframe out;
            out.brightness = previous.brightness + ((next.brightness - previous.brightness) * t);
            out.contrast = previous.contrast + ((next.contrast - previous.contrast) * t);
            out.saturation = previous.saturation + ((next.saturation - previous.saturation) * t);
            out.shadowsR = previous.shadowsR + ((next.shadowsR - previous.shadowsR) * t);
            out.shadowsG = previous.shadowsG + ((next.shadowsG - previous.shadowsG) * t);
            out.shadowsB = previous.shadowsB + ((next.shadowsB - previous.shadowsB) * t);
            out.midtonesR = previous.midtonesR + ((next.midtonesR - previous.midtonesR) * t);
            out.midtonesG = previous.midtonesG + ((next.midtonesG - previous.midtonesG) * t);
            out.midtonesB = previous.midtonesB + ((next.midtonesB - previous.midtonesB) * t);
            out.highlightsR = previous.highlightsR + ((next.highlightsR - previous.highlightsR) * t);
            out.highlightsG = previous.highlightsG + ((next.highlightsG - previous.highlightsG) * t);
            out.highlightsB = previous.highlightsB + ((next.highlightsB - previous.highlightsB) * t);
            return out;
        };

        for (int i = normalized.size() - 2; i >= 1; --i) {
            const TimelineClip::GradingKeyframe& current = normalized[i];
            if (!opacityFrames.contains(current.frame) || !current.linearInterpolation) {
                continue;
            }

            const TimelineClip::GradingKeyframe& previous = normalized[i - 1];
            const TimelineClip::GradingKeyframe& next = normalized[i + 1];
            const int64_t span = next.frame - previous.frame;
            if (span <= 0 || !next.linearInterpolation) {
                continue;
            }

            const qreal t = static_cast<qreal>(current.frame - previous.frame) /
                            static_cast<qreal>(span);
            const TimelineClip::GradingKeyframe expected = blended(previous, next, t);
            if (gradingValuesMatch(current, expected)) {
                normalized.removeAt(i);
            }
        }
    }

    clip.gradingKeyframes = normalized;
    if (!clip.gradingKeyframes.isEmpty()) {
        clip.brightness = clip.gradingKeyframes.constFirst().brightness;
        clip.contrast = clip.gradingKeyframes.constFirst().contrast;
        clip.saturation = clip.gradingKeyframes.constFirst().saturation;
    }
}

void normalizeClipOpacityKeyframes(TimelineClip& clip) {
    std::sort(clip.opacityKeyframes.begin(), clip.opacityKeyframes.end(),
              [](const TimelineClip::OpacityKeyframe& a, const TimelineClip::OpacityKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::OpacityKeyframe> normalized;
    normalized.reserve(clip.opacityKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::OpacityKeyframe keyframe : clip.opacityKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.opacity = qBound<qreal>(0.0, keyframe.opacity, 1.0);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }

    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::OpacityKeyframe keyframe;
            keyframe.frame = 0;
            keyframe.opacity = clip.opacity;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            TimelineClip::OpacityKeyframe firstKeyframe = normalized.constFirst();
            firstKeyframe.frame = 0;
            normalized.push_front(firstKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }

    clip.opacityKeyframes = normalized;
    if (!clip.opacityKeyframes.isEmpty()) {
        clip.opacity = clip.opacityKeyframes.constFirst().opacity;
    }
}

void normalizeClipTitleKeyframes(TimelineClip& clip) {
    std::sort(clip.titleKeyframes.begin(), clip.titleKeyframes.end(),
              [](const TimelineClip::TitleKeyframe& a, const TimelineClip::TitleKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::TitleKeyframe> normalized;
    normalized.reserve(clip.titleKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::TitleKeyframe keyframe : clip.titleKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }
    clip.titleKeyframes = normalized;
}

TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe state;
    if (clip.transformKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.transformKeyframes.constFirst().frame) {
        return clip.transformKeyframes.constFirst();
    }

    for (int i = 1; i < clip.transformKeyframes.size(); ++i) {
        const TimelineClip::TransformKeyframe& previous = clip.transformKeyframes[i - 1];
        const TimelineClip::TransformKeyframe& current = clip.transformKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = qBound<qreal>(0.0,
                                          interpolationFactorForTransformFrames(
                                              clip,
                                              static_cast<qreal>(previous.frame),
                                              static_cast<qreal>(current.frame),
                                              static_cast<qreal>(localFrame)),
                                          1.0);
            state.frame = localFrame;
            state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
            state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
            state.rotation = previous.rotation + ((current.rotation - previous.rotation) * t);
            state.scaleX = previous.scaleX + ((current.scaleX - previous.scaleX) * t);
            state.scaleY = previous.scaleY + ((current.scaleY - previous.scaleY) * t);
            state.linearInterpolation = current.linearInterpolation;
            return state;
        }
        if (localFrame == current.frame) {
            return current;
        }
    }

    return clip.transformKeyframes.constLast();
}

TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe effective = evaluateClipKeyframeOffsetAtFrame(clip, timelineFrame);
    effective.translationX += clip.baseTranslationX;
    effective.translationY += clip.baseTranslationY;
    effective.rotation += clip.baseRotation;
    effective.scaleX = sanitizeScaleValue(clip.baseScaleX * effective.scaleX);
    effective.scaleY = sanitizeScaleValue(clip.baseScaleY * effective.scaleY);
    return effective;
}

TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    TimelineClip::TransformKeyframe state;
    if (clip.transformKeyframes.isEmpty()) {
        state.translationX = clip.baseTranslationX;
        state.translationY = clip.baseTranslationY;
        state.rotation = clip.baseRotation;
        state.scaleX = sanitizeScaleValue(clip.baseScaleX);
        state.scaleY = sanitizeScaleValue(clip.baseScaleY);
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.transformKeyframes.constFirst().frame)) {
        state = clip.transformKeyframes.constFirst();
    } else {
        state = clip.transformKeyframes.constLast();
        for (int i = 1; i < clip.transformKeyframes.size(); ++i) {
            const TimelineClip::TransformKeyframe& previous = clip.transformKeyframes[i - 1];
            const TimelineClip::TransformKeyframe& current = clip.transformKeyframes[i];
            if (localFrame < static_cast<qreal>(current.frame)) {
                if (!current.linearInterpolation || current.frame <= previous.frame) {
                    state = previous;
                } else {
                    const qreal t = qBound<qreal>(0.0,
                                                  interpolationFactorForTransformFrames(
                                                      clip,
                                                      static_cast<qreal>(previous.frame),
                                                      static_cast<qreal>(current.frame),
                                                      localFrame),
                                                  1.0);
                    state.frame = qRound64(localFrame);
                    state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
                    state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
                    state.rotation = previous.rotation + ((current.rotation - previous.rotation) * t);
                    state.scaleX = previous.scaleX + ((current.scaleX - previous.scaleX) * t);
                    state.scaleY = previous.scaleY + ((current.scaleY - previous.scaleY) * t);
                    state.linearInterpolation = current.linearInterpolation;
                }
                break;
            }
            if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
                state = current;
                break;
            }
        }
    }

    state.translationX += clip.baseTranslationX;
    state.translationY += clip.baseTranslationY;
    state.rotation += clip.baseRotation;
    state.scaleX = sanitizeScaleValue(clip.baseScaleX * state.scaleX);
    state.scaleY = sanitizeScaleValue(clip.baseScaleY * state.scaleY);
    return state;
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe state;
    state.rotation = 0.0;
    state.scaleX = 1.0;
    state.scaleY = 1.0;
    if (!clip.speakerFramingEnabled || clip.speakerFramingKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(
        0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.speakerFramingKeyframes.constFirst().frame) {
        return clip.speakerFramingKeyframes.constFirst();
    }
    for (int i = 1; i < clip.speakerFramingKeyframes.size(); ++i) {
        const TimelineClip::TransformKeyframe& previous = clip.speakerFramingKeyframes[i - 1];
        const TimelineClip::TransformKeyframe& current = clip.speakerFramingKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = qBound<qreal>(
                0.0, static_cast<qreal>(localFrame - previous.frame) /
                         static_cast<qreal>(current.frame - previous.frame), 1.0);
            state.frame = localFrame;
            state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
            state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
            state.rotation = 0.0;
            state.scaleX = sanitizeScaleValue(previous.scaleX + ((current.scaleX - previous.scaleX) * t));
            state.scaleY = sanitizeScaleValue(previous.scaleY + ((current.scaleY - previous.scaleY) * t));
            state.linearInterpolation = current.linearInterpolation;
            return state;
        }
        if (localFrame == current.frame) {
            return current;
        }
    }
    return clip.speakerFramingKeyframes.constLast();
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,
                                                                     qreal timelineFramePosition) {
    TimelineClip::TransformKeyframe state;
    state.rotation = 0.0;
    state.scaleX = 1.0;
    state.scaleY = 1.0;
    if (!clip.speakerFramingEnabled || clip.speakerFramingKeyframes.isEmpty()) {
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(
        0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.speakerFramingKeyframes.constFirst().frame)) {
        return clip.speakerFramingKeyframes.constFirst();
    }
    state = clip.speakerFramingKeyframes.constLast();
    for (int i = 1; i < clip.speakerFramingKeyframes.size(); ++i) {
        const TimelineClip::TransformKeyframe& previous = clip.speakerFramingKeyframes[i - 1];
        const TimelineClip::TransformKeyframe& current = clip.speakerFramingKeyframes[i];
        if (localFrame < static_cast<qreal>(current.frame)) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                state = previous;
            } else {
                const qreal t = qBound<qreal>(
                    0.0, (localFrame - static_cast<qreal>(previous.frame)) /
                             static_cast<qreal>(current.frame - previous.frame), 1.0);
                state.frame = qRound64(localFrame);
                state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
                state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
                state.rotation = 0.0;
                state.scaleX = sanitizeScaleValue(previous.scaleX + ((current.scaleX - previous.scaleX) * t));
                state.scaleY = sanitizeScaleValue(previous.scaleY + ((current.scaleY - previous.scaleY) * t));
                state.linearInterpolation = current.linearInterpolation;
            }
            break;
        }
        if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
            state = current;
            break;
        }
    }
    return state;
}

TimelineClip::TransformKeyframe composeClipTransforms(const TimelineClip::TransformKeyframe& base,
                                                      const TimelineClip::TransformKeyframe& overlay) {
    TimelineClip::TransformKeyframe composed = base;
    composed.translationX += overlay.translationX;
    composed.translationY += overlay.translationY;
    composed.rotation += overlay.rotation;
    composed.scaleX = sanitizeScaleValue(base.scaleX * overlay.scaleX);
    composed.scaleY = sanitizeScaleValue(base.scaleY * overlay.scaleY);
    return composed;
}

TimelineClip::TransformKeyframe evaluateClipRenderTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    return composeClipTransforms(
        evaluateClipTransformAtFrame(clip, timelineFrame),
        evaluateClipSpeakerFramingAtFrame(clip, timelineFrame));
}

TimelineClip::TransformKeyframe evaluateClipRenderTransformAtPosition(const TimelineClip& clip,
                                                                      qreal timelineFramePosition) {
    return composeClipTransforms(
        evaluateClipTransformAtPosition(clip, timelineFramePosition),
        evaluateClipSpeakerFramingAtPosition(clip, timelineFramePosition));
}

TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
    state.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
    if (clip.gradingKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.gradingKeyframes.constFirst().frame) {
        TimelineClip::GradingKeyframe first = clip.gradingKeyframes.constFirst();
        first.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
        return first;
    }

    for (int i = 1; i < clip.gradingKeyframes.size(); ++i) {
        const TimelineClip::GradingKeyframe& previous = clip.gradingKeyframes[i - 1];
        const TimelineClip::GradingKeyframe& current = clip.gradingKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = localFrame;
            state.brightness = previous.brightness + ((current.brightness - previous.brightness) * t);
            state.contrast = previous.contrast + ((current.contrast - previous.contrast) * t);
            state.saturation = previous.saturation + ((current.saturation - previous.saturation) * t);
            // Shadows/Midtones/Highlights interpolation
            state.shadowsR = previous.shadowsR + ((current.shadowsR - previous.shadowsR) * t);
            state.shadowsG = previous.shadowsG + ((current.shadowsG - previous.shadowsG) * t);
            state.shadowsB = previous.shadowsB + ((current.shadowsB - previous.shadowsB) * t);
            state.midtonesR = previous.midtonesR + ((current.midtonesR - previous.midtonesR) * t);
            state.midtonesG = previous.midtonesG + ((current.midtonesG - previous.midtonesG) * t);
            state.midtonesB = previous.midtonesB + ((current.midtonesB - previous.midtonesB) * t);
            state.highlightsR = previous.highlightsR + ((current.highlightsR - previous.highlightsR) * t);
            state.highlightsG = previous.highlightsG + ((current.highlightsG - previous.highlightsG) * t);
            state.highlightsB = previous.highlightsB + ((current.highlightsB - previous.highlightsB) * t);
            state.linearInterpolation = current.linearInterpolation;
            state.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
            return state;
        }
        if (localFrame == current.frame) {
            TimelineClip::GradingKeyframe resolved = current;
            resolved.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
            return resolved;
        }
    }

    TimelineClip::GradingKeyframe last = clip.gradingKeyframes.constLast();
    last.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
    return last;
}

TimelineClip::GradingKeyframe evaluateClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
    state.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
    if (clip.gradingKeyframes.isEmpty()) {
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.gradingKeyframes.constFirst().frame)) {
        TimelineClip::GradingKeyframe first = clip.gradingKeyframes.constFirst();
        first.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
        return first;
    }

    state = clip.gradingKeyframes.constLast();
    for (int i = 1; i < clip.gradingKeyframes.size(); ++i) {
        const TimelineClip::GradingKeyframe& previous = clip.gradingKeyframes[i - 1];
        const TimelineClip::GradingKeyframe& current = clip.gradingKeyframes[i];
        if (localFrame < static_cast<qreal>(current.frame)) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = qRound64(localFrame);
            state.brightness = previous.brightness + ((current.brightness - previous.brightness) * t);
            state.contrast = previous.contrast + ((current.contrast - previous.contrast) * t);
            state.saturation = previous.saturation + ((current.saturation - previous.saturation) * t);
            // Shadows/Midtones/Highlights interpolation
            state.shadowsR = previous.shadowsR + ((current.shadowsR - previous.shadowsR) * t);
            state.shadowsG = previous.shadowsG + ((current.shadowsG - previous.shadowsG) * t);
            state.shadowsB = previous.shadowsB + ((current.shadowsB - previous.shadowsB) * t);
            state.midtonesR = previous.midtonesR + ((current.midtonesR - previous.midtonesR) * t);
            state.midtonesG = previous.midtonesG + ((current.midtonesG - previous.midtonesG) * t);
            state.midtonesB = previous.midtonesB + ((current.midtonesB - previous.midtonesB) * t);
            state.highlightsR = previous.highlightsR + ((current.highlightsR - previous.highlightsR) * t);
            state.highlightsG = previous.highlightsG + ((current.highlightsG - previous.highlightsG) * t);
            state.highlightsB = previous.highlightsB + ((current.highlightsB - previous.highlightsB) * t);
            state.linearInterpolation = current.linearInterpolation;
            state.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
            return state;
        }
        if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
            TimelineClip::GradingKeyframe resolved = current;
            resolved.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
            return resolved;
        }
    }
    state.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
    return state;
}

qreal evaluateClipOpacityAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    if (clip.opacityKeyframes.isEmpty()) {
        return qBound<qreal>(0.0, clip.opacity, 1.0);
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.opacityKeyframes.constFirst().frame) {
        return qBound<qreal>(0.0, clip.opacityKeyframes.constFirst().opacity, 1.0);
    }

    for (int i = 1; i < clip.opacityKeyframes.size(); ++i) {
        const TimelineClip::OpacityKeyframe& previous = clip.opacityKeyframes[i - 1];
        const TimelineClip::OpacityKeyframe& current = clip.opacityKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return qBound<qreal>(0.0, previous.opacity, 1.0);
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            return qBound<qreal>(0.0, previous.opacity + ((current.opacity - previous.opacity) * t), 1.0);
        }
        if (localFrame == current.frame) {
            return qBound<qreal>(0.0, current.opacity, 1.0);
        }
    }
    return qBound<qreal>(0.0, clip.opacityKeyframes.constLast().opacity, 1.0);
}

qreal evaluateClipOpacityAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    if (clip.opacityKeyframes.isEmpty()) {
        return qBound<qreal>(0.0, clip.opacity, 1.0);
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.opacityKeyframes.constFirst().frame)) {
        return qBound<qreal>(0.0, clip.opacityKeyframes.constFirst().opacity, 1.0);
    }

    for (int i = 1; i < clip.opacityKeyframes.size(); ++i) {
        const TimelineClip::OpacityKeyframe& previous = clip.opacityKeyframes[i - 1];
        const TimelineClip::OpacityKeyframe& current = clip.opacityKeyframes[i];
        if (localFrame < static_cast<qreal>(current.frame)) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return qBound<qreal>(0.0, previous.opacity, 1.0);
            }
            const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                            static_cast<qreal>(current.frame - previous.frame);
            return qBound<qreal>(0.0, previous.opacity + ((current.opacity - previous.opacity) * t), 1.0);
        }
        if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
            return qBound<qreal>(0.0, current.opacity, 1.0);
        }
    }
    return qBound<qreal>(0.0, clip.opacityKeyframes.constLast().opacity, 1.0);
}
