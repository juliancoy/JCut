#include "transform_skip_aware_timing.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace {
struct TranscriptSpeechRange {
    qreal startFrame = 0.0;
    qreal endFrameExclusive = 0.0;
};

QMutex& transformTimelineRangesMutex() {
    static QMutex mutex;
    return mutex;
}

QVector<ExportRangeSegment>& transformTimelineRangesStorage() {
    static QVector<ExportRangeSegment> ranges;
    return ranges;
}

QVector<TranscriptSpeechRange> loadTranscriptSpeechRanges(const QString& transcriptPath) {
    static QMutex cacheMutex;
    static QHash<QString, QVector<TranscriptSpeechRange>> cachedRangesByKey;

    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return {};
    }

    const QString cacheKey = info.absoluteFilePath() + QLatin1Char('|') +
                             QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cachedRangesByKey.constFind(cacheKey);
        if (it != cachedRangesByKey.cend()) {
            return it.value();
        }
    }

    QFile transcriptFile(info.absoluteFilePath());
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        return {};
    }

    QVector<TranscriptSpeechRange> ranges;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonArray words = segmentValue.toObject().value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const qreal startFrame = qMax<qreal>(0.0, startSeconds * static_cast<qreal>(kTimelineFps));
            const qreal endFrameExclusive =
                qMax(startFrame + (1.0 / static_cast<qreal>(kTimelineFps)),
                     endSeconds * static_cast<qreal>(kTimelineFps));
            ranges.push_back({startFrame, endFrameExclusive});
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const TranscriptSpeechRange& a, const TranscriptSpeechRange& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrameExclusive < b.endFrameExclusive;
        }
        return a.startFrame < b.startFrame;
    });

    QVector<TranscriptSpeechRange> merged;
    for (const TranscriptSpeechRange& range : std::as_const(ranges)) {
        if (merged.isEmpty() || range.startFrame > (merged.constLast().endFrameExclusive + 0.000001)) {
            merged.push_back(range);
        } else {
            merged.last().endFrameExclusive =
                qMax(merged.constLast().endFrameExclusive, range.endFrameExclusive);
        }
    }

    {
        QMutexLocker locker(&cacheMutex);
        cachedRangesByKey.insert(cacheKey, merged);
    }
    return merged;
}

qreal effectiveDisplayedDurationBetweenFrames(const QVector<TranscriptSpeechRange>& ranges,
                                              qreal startFrame,
                                              qreal endFrame,
                                              qreal framePosition) {
    if (framePosition <= startFrame) {
        return 0.0;
    }

    const qreal boundedFrame = qBound(startFrame, framePosition, endFrame);
    qreal effective = 0.0;

    for (const TranscriptSpeechRange& range : ranges) {
        const qreal overlapStart = qMax(startFrame, static_cast<qreal>(range.startFrame));
        const qreal overlapEnd = qMin(boundedFrame, static_cast<qreal>(range.endFrameExclusive));

        if (overlapEnd > overlapStart) {
            effective += overlapEnd - overlapStart;
        }

        if (static_cast<qreal>(range.startFrame) >= boundedFrame) {
            break;
        }
    }

    return effective;
}

qreal effectiveDurationBeforeFrame(const QVector<TranscriptSpeechRange>& ranges,
                                   qreal clipStartSourceFrame,
                                   qreal clipEndSourceFrame,
                                   qreal framePosition) {
    if (framePosition <= clipStartSourceFrame) {
        return 0.0;
    }

    const qreal boundedFrame = qBound(clipStartSourceFrame, framePosition, clipEndSourceFrame);
    qreal effective = 0.0;
    for (const TranscriptSpeechRange& range : ranges) {
        const qreal overlapStart = qMax(clipStartSourceFrame, range.startFrame);
        const qreal overlapEnd = qMin(boundedFrame, range.endFrameExclusive);
        if (overlapEnd > overlapStart) {
            effective += overlapEnd - overlapStart;
        }
        if (range.startFrame >= boundedFrame) {
            break;
        }
    }
    return effective;
}

qreal effectiveTimelineDurationBeforeFrame(const QVector<ExportRangeSegment>& ranges,
                                           qreal framePosition) {
    if (ranges.isEmpty()) {
        return 0.0;
    }

    qreal effective = 0.0;
    for (const ExportRangeSegment& range : ranges) {
        const qreal startFrame = static_cast<qreal>(range.startFrame);
        const qreal endFrameExclusive = static_cast<qreal>(range.endFrame) + 1.0;
        if (framePosition <= startFrame) {
            break;
        }
        const qreal overlapEnd = qMin(framePosition, endFrameExclusive);
        if (overlapEnd > startFrame) {
            effective += overlapEnd - startFrame;
        }
        if (framePosition < endFrameExclusive) {
            break;
        }
    }
    return effective;
}

QVector<ExportRangeSegment> activeTransformTimelineRanges() {
    QMutexLocker locker(&transformTimelineRangesMutex());
    return transformTimelineRangesStorage();
}
}  // namespace

void setTransformSkipAwareTimelineRanges(const QVector<ExportRangeSegment>& ranges) {
    QVector<ExportRangeSegment> normalized = ranges;
    std::sort(normalized.begin(), normalized.end(), [](const ExportRangeSegment& a,
                                                       const ExportRangeSegment& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });

    QVector<ExportRangeSegment> merged;
    merged.reserve(normalized.size());
    for (const ExportRangeSegment& range : std::as_const(normalized)) {
        if (range.endFrame < range.startFrame) {
            continue;
        }
        if (merged.isEmpty() || range.startFrame > (merged.constLast().endFrame + 1)) {
            merged.push_back(range);
            continue;
        }
        merged.last().endFrame = qMax(merged.constLast().endFrame, range.endFrame);
    }

    QMutexLocker locker(&transformTimelineRangesMutex());
    transformTimelineRangesStorage() = merged;
}

qreal interpolationFactorForTransformFrames(const TimelineClip& clip,
                                            qreal startLocalFrame,
                                            qreal endLocalFrame,
                                            qreal localFrame) {
    if (endLocalFrame <= startLocalFrame) {
        return 0.0;
    }

    const qreal boundedLocalFrame = qBound(startLocalFrame, localFrame, endLocalFrame);
    const qreal baseT = (boundedLocalFrame - startLocalFrame) / (endLocalFrame - startLocalFrame);
    if (!clip.transformSkipAwareTiming || clip.filePath.isEmpty()) {
        return baseT;
    }

    const QVector<ExportRangeSegment> timelineRanges = activeTransformTimelineRanges();
    if (!timelineRanges.isEmpty()) {
        const qreal startTimelineFrame = static_cast<qreal>(clip.startFrame) + qMax<qreal>(0.0, startLocalFrame);
        const qreal endTimelineFrame = static_cast<qreal>(clip.startFrame) + qMax<qreal>(0.0, endLocalFrame);
        const qreal currentTimelineFrame =
            static_cast<qreal>(clip.startFrame) + qMax<qreal>(0.0, boundedLocalFrame);
        const qreal effectiveStart = effectiveTimelineDurationBeforeFrame(timelineRanges, startTimelineFrame);
        const qreal effectiveEnd = effectiveTimelineDurationBeforeFrame(timelineRanges, endTimelineFrame);
        const qreal effectiveLocal = effectiveTimelineDurationBeforeFrame(timelineRanges, currentTimelineFrame);
        const qreal effectiveSpan = effectiveEnd - effectiveStart;
        if (effectiveSpan <= 0.000001) {
            return 0.0;
        }
        return qBound<qreal>(0.0, (effectiveLocal - effectiveStart) / effectiveSpan, 1.0);
    }

    const QVector<TranscriptSpeechRange> ranges = loadTranscriptSpeechRanges(
        activeTranscriptPathForClipFile(clip.filePath));
    if (ranges.isEmpty()) {
        return baseT;
    }

    const qreal clipStartSourceFrame = static_cast<qreal>(clip.sourceInFrame);
    const qreal clipEndSourceFrame =
        clipStartSourceFrame + qMax<qreal>(1.0, static_cast<qreal>(clip.durationFrames));
    const qreal startSourceFrame = clipStartSourceFrame + qMax<qreal>(0.0, startLocalFrame);
    const qreal endSourceFrame = clipStartSourceFrame + qMax<qreal>(0.0, endLocalFrame);
    const qreal sourceFrame = clipStartSourceFrame + qMax<qreal>(0.0, boundedLocalFrame);

    const qreal effectiveStart =
        effectiveDurationBeforeFrame(ranges, clipStartSourceFrame, clipEndSourceFrame, startSourceFrame);
    const qreal effectiveEnd =
        effectiveDurationBeforeFrame(ranges, clipStartSourceFrame, clipEndSourceFrame, endSourceFrame);
    const qreal effectiveLocal =
        effectiveDurationBeforeFrame(ranges, clipStartSourceFrame, clipEndSourceFrame, sourceFrame);
    const qreal effectiveSpan = effectiveEnd - effectiveStart;
    if (effectiveSpan <= 0.000001) {
        return 0.0;
    }

    return qBound<qreal>(0.0, (effectiveLocal - effectiveStart) / effectiveSpan, 1.0);
}
