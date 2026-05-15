#include "editor_shared.h"

#include <QtGlobal>

#include <cmath>

int64_t frameToSamples(int64_t frame) {
    return qMax<int64_t>(0, frame * kSamplesPerFrame);
}

qreal samplesToFramePosition(int64_t samples) {
    return static_cast<qreal>(samples) / static_cast<qreal>(kSamplesPerFrame);
}

qreal resolvedSourceFps(const TimelineClip& clip) {
    const qreal fps = clip.sourceFps;
    if (!std::isfinite(fps) || fps <= 0.001) {
        return static_cast<qreal>(kTimelineFps);
    }
    return fps;
}

qreal timelineFrameToSeconds(int64_t timelineFrame) {
    return static_cast<qreal>(qMax<int64_t>(0, timelineFrame)) / static_cast<qreal>(kTimelineFps);
}

QRectF normalizedCenterBoxRect(qreal xNorm, qreal yNorm, qreal boxSizeNorm, const QSizeF& frameSizePx) {
    if (frameSizePx.width() <= 1.0 || frameSizePx.height() <= 1.0) {
        return QRectF();
    }
    const qreal boundedX = qBound<qreal>(0.0, xNorm, 1.0);
    const qreal boundedY = qBound<qreal>(0.0, yNorm, 1.0);
    const qreal boundedSize = qBound<qreal>(0.0, boxSizeNorm, 1.0);
    if (boundedSize <= 0.0) {
        return QRectF();
    }

    const qreal minSidePx = qMax<qreal>(1.0, qMin(frameSizePx.width(), frameSizePx.height()));
    const qreal sidePx = qBound<qreal>(1.0, boundedSize * minSidePx, minSidePx);
    const qreal halfXNorm = 0.5 * (sidePx / frameSizePx.width());
    const qreal halfYNorm = 0.5 * (sidePx / frameSizePx.height());
    const qreal left = qBound<qreal>(0.0, boundedX - halfXNorm, 1.0);
    const qreal top = qBound<qreal>(0.0, boundedY - halfYNorm, 1.0);
    const qreal right = qBound<qreal>(0.0, boundedX + halfXNorm, 1.0);
    const qreal bottom = qBound<qreal>(0.0, boundedY + halfYNorm, 1.0);
    return QRectF(QPointF(left, top), QPointF(right, bottom))
        .normalized()
        .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

int64_t sourceFramesToSamples(const TimelineClip& clip, qreal sourceFrames) {
    const qreal clampedSourceFrames = qMax<qreal>(0.0, sourceFrames);
    const qreal durationSeconds = clampedSourceFrames / resolvedSourceFps(clip);
    return qMax<int64_t>(0, qRound64(durationSeconds * static_cast<qreal>(kAudioSampleRate)));
}

int64_t clipTimelineStartSamples(const TimelineClip& clip) {
    return frameToSamples(clip.startFrame) + clip.startSubframeSamples;
}

int64_t clipSourceInSamples(const TimelineClip& clip) {
    return sourceFramesToSamples(clip, static_cast<qreal>(clip.sourceInFrame)) + clip.sourceInSubframeSamples;
}

int64_t playableSampleAtOrAfter(int64_t samplePos,
                                const QVector<ExportRangeSegment>& ranges,
                                bool* atOrPastEnd) {
    if (atOrPastEnd) {
        *atOrPastEnd = false;
    }
    if (ranges.isEmpty()) {
        return qMax<int64_t>(0, samplePos);
    }

    const int64_t boundedSample = qMax<int64_t>(0, samplePos);
    for (const ExportRangeSegment& range : ranges) {
        const int64_t startSample = frameToSamples(range.startFrame);
        const int64_t endSampleExclusive = frameToSamples(range.endFrame + 1);
        if (boundedSample < startSample) {
            return startSample;
        }
        if (boundedSample >= startSample && boundedSample < endSampleExclusive) {
            return boundedSample;
        }
    }

    if (atOrPastEnd) {
        *atOrPastEnd = true;
    }
    return qMax<int64_t>(0, frameToSamples(ranges.constLast().endFrame + 1) - 1);
}

void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples) {
    if (subframeSamples >= kSamplesPerFrame) {
        frame += subframeSamples / kSamplesPerFrame;
        subframeSamples %= kSamplesPerFrame;
    }
    while (subframeSamples < 0 && frame > 0) {
        --frame;
        subframeSamples += kSamplesPerFrame;
    }
    if (frame <= 0) {
        frame = 0;
        subframeSamples = qMax<int64_t>(0, subframeSamples);
    }
}

void normalizeClipTiming(TimelineClip& clip) {
    normalizeSubframeTiming(clip.startFrame, clip.startSubframeSamples);
    normalizeSubframeTiming(clip.sourceInFrame, clip.sourceInSubframeSamples);
    clip.playbackRate = qBound<qreal>(0.001, clip.playbackRate, 1000.0);
}

QString transformInterpolationLabel(bool linearInterpolation) {
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}
